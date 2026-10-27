#include <mln/storage/file_source_manager.hpp>
#include <mln/storage/file_source_request.hpp>
#include <mln/storage/pmtiles_file_source.hpp>
#include <mln/storage/resource.hpp>
#include <mln/storage/resource_options.hpp>
#include <mln/storage/response.hpp>
#include <mln/util/client_options.hpp>
#include <mln/util/platform.hpp>
#include <mln/util/run_loop.hpp>

#include <pmtiles.hpp>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>

#include <climits>
#include <gtest/gtest.h>

namespace {

std::string toAbsoluteURL(const std::string &fileName) {
    auto path = std::filesystem::current_path() / "test/fixtures/storage/pmtiles" / fileName;
    return std::string(mln::util::PMTILES_PROTOCOL) + std::string(mln::util::FILE_PROTOCOL) + path.string();
}

} // namespace

using namespace mln;

namespace {

const std::string archiveURL = "pmtiles://archive.pmtiles";

// A PMTiles archive with a leaf directory per tile, so looking up a tile reads the root directory
// and then the tile's own leaf directory. The tile with PMTiles tile ID `id` holds "tile <id>".
std::string makeLeafPerTileArchive(uint64_t tileCount) {
    std::vector<pmtiles::entryv3> entries;
    std::string tileData;
    for (uint64_t id = 0; id < tileCount; ++id) {
        const std::string data = "tile " + std::to_string(id);
        entries.emplace_back(id, tileData.size(), static_cast<uint32_t>(data.size()), 1);
        tileData += data;
    }

    const auto directories = pmtiles::build_root_leaves(
        [](const std::string &data, uint8_t) { return data; }, pmtiles::COMPRESSION_NONE, entries, 1);
    const std::string &rootDirectory = std::get<0>(directories);
    const std::string &leafDirectories = std::get<1>(directories);

    pmtiles::headerv3 header{};
    header.root_dir_offset = 127;
    header.root_dir_bytes = rootDirectory.size();
    header.json_metadata_offset = header.root_dir_offset + header.root_dir_bytes;
    header.leaf_dirs_offset = header.json_metadata_offset;
    header.leaf_dirs_bytes = leafDirectories.size();
    header.tile_data_offset = header.leaf_dirs_offset + header.leaf_dirs_bytes;
    header.tile_data_bytes = tileData.size();
    header.addressed_tiles_count = header.tile_entries_count = header.tile_contents_count = tileCount;
    header.internal_compression = pmtiles::COMPRESSION_NONE;
    header.tile_compression = pmtiles::COMPRESSION_NONE;
    header.tile_type = pmtiles::TILETYPE_PNG;
    header.max_zoom = pmtiles::tileid_to_zxy(tileCount - 1).z;

    return header.serialize() + rootDirectory + leafDirectories + tileData;
}

Resource tileResource(uint64_t id,
                      const std::string &url = archiveURL,
                      Resource::LoadingMethod loadingMethod = Resource::LoadingMethod::All) {
    const auto tile = pmtiles::tileid_to_zxy(id);
    return Resource::tile(url,
                          1.0,
                          static_cast<int32_t>(tile.x),
                          static_cast<int32_t>(tile.y),
                          static_cast<int8_t>(tile.z),
                          Tileset::Scheme::XYZ,
                          loadingMethod);
}

// Serves the byte ranges PMTilesFileSource fetches from an archive in memory. While holding, it
// keeps every fetch pending until `release`, so a test decides which fetches are in flight together.
class Archive {
public:
    explicit Archive(std::string bytes_)
        : bytes(std::move(bytes_)) {}

    void fetch(const Resource &resource, const ActorRef<FileSourceRequest> &request) {
        std::scoped_lock lock(mutex);
        ++fetches;
        ++openFetches;
        if (holding) {
            held.emplace_back(resource, request);
            changed.notify_all();
        } else {
            respond(resource, request);
        }
    }

    // A fetch is closed when its request is cancelled or freed.
    void close() {
        std::scoped_lock lock(mutex);
        --openFetches;
        changed.notify_all();
    }

    void hold() {
        std::scoped_lock lock(mutex);
        holding = true;
    }

    // Waits until `count` fetches are held.
    bool waitForHeld(std::size_t count) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(10), [&] { return held.size() >= count; });
    }

    // Waits until `count` fetches are open.
    bool waitForOpenFetches(std::size_t count) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(5), [&] { return openFetches == count; });
    }

    // Answers the held fetches in the order they were made, and stops holding.
    void release() {
        std::scoped_lock lock(mutex);
        holding = false;
        for (const auto &[resource, request] : held) {
            respond(resource, request);
        }
        held.clear();
    }

    std::size_t fetchCount() {
        std::scoped_lock lock(mutex);
        return fetches;
    }

private:
    void respond(const Resource &resource, const ActorRef<FileSourceRequest> &request) const {
        Response response;
        const auto [first, last] = resource.dataRange.value();
        response.data = std::make_shared<std::string>(
            bytes.substr(static_cast<std::size_t>(first), static_cast<std::size_t>(last - first + 1)));
        request.invoke(&FileSourceRequest::setResponse, response);
    }

    const std::string bytes;
    std::mutex mutex;
    std::condition_variable changed;
    bool holding = false;
    std::vector<std::pair<Resource, ActorRef<FileSourceRequest>>> held;
    std::size_t fetches = 0;
    std::size_t openFetches = 0;
};

class ArchiveFileSource final : public FileSource {
public:
    explicit ArchiveFileSource(std::shared_ptr<Archive> archive_)
        : archive(std::move(archive_)) {}

    std::unique_ptr<AsyncRequest> request(const Resource &resource, Callback callback) override {
        auto req = std::make_unique<FileSourceRequest>(std::move(callback));
        req->onCancel([source = archive] { source->close(); });
        archive->fetch(resource, req->actor());
        return req;
    }

    bool canRequest(const Resource &) const override { return true; }
    void setResourceOptions(ResourceOptions) override {}
    ResourceOptions getResourceOptions() override { return ResourceOptions(); }
    void setClientOptions(ClientOptions) override {}
    ClientOptions getClientOptions() override { return ClientOptions(); }

private:
    std::shared_ptr<Archive> archive;
};

// While alive, the file source manager makes an ArchiveFileSource for the given type: for the
// resource loader, PMTilesFileSource fetches from it directly; for the network, it stands in for
// the online file source behind the resource loader and the ambient cache.
class ScopedArchiveFileSource {
public:
    ScopedArchiveFileSource(FileSourceType type_, std::shared_ptr<Archive> archive)
        : type(type_),
          previous(FileSourceManager::get()->unRegisterFileSourceFactory(type)) {
        FileSourceManager::get()->registerFileSourceFactory(
            type, [archive](const ResourceOptions &, const ClientOptions &) -> std::unique_ptr<FileSource> {
                return std::make_unique<ArchiveFileSource>(archive);
            });
    }

    ~ScopedArchiveFileSource() {
        if (previous) {
            FileSourceManager::get()->registerFileSourceFactory(type, std::move(previous));
        } else {
            FileSourceManager::get()->unRegisterFileSourceFactory(type);
        }
    }

    ScopedArchiveFileSource(const ScopedArchiveFileSource &) = delete;
    ScopedArchiveFileSource &operator=(const ScopedArchiveFileSource &) = delete;

private:
    const FileSourceType type;
    FileSourceManager::FileSourceFactory previous;
};

// Options of their own, so the file source manager makes new file sources from the factories
// instead of returning ones another test left alive.
ResourceOptions archiveResourceOptions() {
    ResourceOptions options;
    options.withApiKey("PMTilesFileSourceArchiveTest");
    return options;
}

Response requestOnce(util::RunLoop &loop, FileSource &fileSource, const Resource &resource) {
    Response result;
    std::unique_ptr<AsyncRequest> request = fileSource.request(resource, [&](const Response &response) {
        result = response;
        loop.stop();
    });
    loop.run();
    return result;
}

// Makes the requests together: the archive holds what they fetch until each has made its first
// fetch, so none of them finds the others' results in a cache.
std::vector<Response> requestTogether(util::RunLoop &loop,
                                      FileSource &fileSource,
                                      Archive &archive,
                                      const std::vector<Resource> &resources) {
    std::vector<Response> responses(resources.size());
    std::vector<std::unique_ptr<AsyncRequest>> requests;
    std::size_t remaining = resources.size();

    archive.hold();
    for (std::size_t i = 0; i < resources.size(); ++i) {
        requests.push_back(fileSource.request(resources[i], [&, i](const Response &response) {
            responses[i] = response;
            if (--remaining == 0) {
                loop.stop();
            }
        }));
    }
    EXPECT_TRUE(archive.waitForHeld(resources.size()));
    archive.release();
    loop.run();

    return responses;
}

void expectTile(uint64_t id, const Response &response) {
    ASSERT_FALSE(response.error) << "tile " << id << ": " << response.error->message;
    ASSERT_TRUE(response.data) << "tile " << id;
    EXPECT_EQ("tile " + std::to_string(id), *response.data);
}

} // namespace

TEST(PMTilesFileSource, AcceptsURL) {
    PMTilesFileSource pmtiles(ResourceOptions::Default(), ClientOptions());
    EXPECT_TRUE(pmtiles.canRequest(Resource::style("pmtiles:///test")));
    EXPECT_FALSE(pmtiles.canRequest(Resource::style("pmtile://test")));
    EXPECT_FALSE(pmtiles.canRequest(Resource::style("pmtiles:")));
    EXPECT_FALSE(pmtiles.canRequest(Resource::style("")));
}

// Nonexistent pmtiles file raises error
TEST(PMTilesFileSource, NonExistentFile) {
    util::RunLoop loop;

    PMTilesFileSource pmtiles(ResourceOptions::Default(), ClientOptions());

    std::unique_ptr<AsyncRequest> req = pmtiles.request(
        {Resource::Unknown, toAbsoluteURL("does_not_exist")}, [&](Response res) {
            req.reset();
            ASSERT_NE(nullptr, res.error);
            EXPECT_EQ(Response::Error::Reason::NotFound, res.error->reason);
            EXPECT_NE((res.error->message).find("path not found"), std::string::npos);
            ASSERT_FALSE(res.data.get());
            loop.stop();
        });

    loop.run();
}

// Existing pmtiles file default request returns TileJSON
TEST(PMTilesFileSource, TileJSON) {
    util::RunLoop loop;

    PMTilesFileSource pmtiles(ResourceOptions::Default(), ClientOptions());

    std::unique_ptr<AsyncRequest> req = pmtiles.request(
        {Resource::Unknown, toAbsoluteURL("geography-class-png.pmtiles")}, [&](Response res) {
            req.reset();
            EXPECT_EQ(nullptr, res.error);
            ASSERT_TRUE(res.data.get());
            // basic test that TileJSON included a tile URL
            EXPECT_NE((*res.data).find("geography-class-png.pmtiles"), std::string::npos);
            loop.stop();
        });

    loop.run();
}

// Existing tiles return tile data
TEST(PMTilesFileSource, Tile) {
    util::RunLoop loop;

    PMTilesFileSource pmtiles(ResourceOptions::Default(), ClientOptions());

    std::unique_ptr<AsyncRequest> req = pmtiles.request(
        Resource::tile(toAbsoluteURL("geography-class-png.pmtiles"), 1.0, 0, 0, 0, Tileset::Scheme::XYZ),
        [&](Response res) {
            req.reset();
            EXPECT_EQ(nullptr, res.error);
            ASSERT_TRUE(res.data.get());
            ASSERT_EQ(res.noContent, false);
            loop.stop();
        });

    loop.run();
}

// Nonexistent tiles do not raise errors, they simply return no content
TEST(PMTilesFileSource, NonExistentTile) {
    util::RunLoop loop;

    PMTilesFileSource pmtiles(ResourceOptions::Default(), ClientOptions());

    std::unique_ptr<AsyncRequest> req = pmtiles.request(
        Resource::tile(toAbsoluteURL("geography-class-png.pmtiles"), 1.0, 0, 0, 4, Tileset::Scheme::XYZ),
        [&](Response res) {
            req.reset();
            EXPECT_EQ(nullptr, res.error);
            ASSERT_FALSE(res.data.get());
            ASSERT_EQ(res.noContent, true);
            loop.stop();
        });

    loop.run();
}

// A tile whose bytes start with gzip magic but are otherwise corrupt must yield an error
// response — the decompression failure must not propagate as an exception.
TEST(PMTilesFileSource, CorruptGzipTile) {
    util::RunLoop loop;

    PMTilesFileSource pmtiles(ResourceOptions::Default(), ClientOptions());

    std::unique_ptr<AsyncRequest> req = pmtiles.request(
        Resource::tile(toAbsoluteURL("corrupt-gzip-tile.pmtiles"), 1.0, 0, 0, 0, Tileset::Scheme::XYZ),
        [&](Response res) {
            req.reset();
            ASSERT_NE(nullptr, res.error);
            EXPECT_EQ(Response::Error::Reason::Other, res.error->reason);
            EXPECT_NE(res.error->message.find("Error decompressing PMTiles tile:"), std::string::npos);
            loop.stop();
        });

    loop.run();
}

// Metadata whose bytes start with gzip magic but are otherwise corrupt must yield an error
// response — the decompression failure must not propagate as an exception. Unlike the tile path,
// this one runs on the PMTiles file source thread with no handler above it, so an escaping
// exception calls std::terminate and aborts the process.
TEST(PMTilesFileSource, CorruptGzipMetadata) {
    util::RunLoop loop;

    PMTilesFileSource pmtiles(ResourceOptions::Default(), ClientOptions());

    std::unique_ptr<AsyncRequest> req = pmtiles.request(
        {Resource::Unknown, toAbsoluteURL("corrupt-gzip-metadata.pmtiles")}, [&](Response res) {
            req.reset();
            ASSERT_NE(nullptr, res.error);
            EXPECT_EQ(Response::Error::Reason::Other, res.error->reason);
            EXPECT_NE(res.error->message.find("Error decompressing PMTiles metadata:"), std::string::npos);
            loop.stop();
        });

    loop.run();
}

// Regression test for https://github.com/maplibre/maplibre-native/issues/3690 — the loading
// method for PMTiles sub-requests changed from `Network` to `All` so that http(s)-hosted
// archives flow through the ambient cache. The file:// path bypasses the cache by design,
// so this test can only confirm that issuing the same tile twice still succeeds; a true
// cache-hit assertion requires an http(s) PMTiles fixture and a mock server.
TEST(PMTilesFileSource, RepeatedTileRequest) {
    util::RunLoop loop;

    PMTilesFileSource pmtiles(ResourceOptions::Default(), ClientOptions());

    std::unique_ptr<AsyncRequest> req1;
    std::unique_ptr<AsyncRequest> req2;

    req1 = pmtiles.request(
        Resource::tile(toAbsoluteURL("geography-class-png.pmtiles"), 1.0, 0, 0, 0, Tileset::Scheme::XYZ),
        [&](Response res1) {
            req1.reset();
            ASSERT_EQ(nullptr, res1.error);
            ASSERT_TRUE(res1.data.get());
            const auto firstSize = res1.data->size();

            req2 = pmtiles.request(
                Resource::tile(toAbsoluteURL("geography-class-png.pmtiles"), 1.0, 0, 0, 0, Tileset::Scheme::XYZ),
                [&, firstSize](Response res2) {
                    req2.reset();
                    EXPECT_EQ(nullptr, res2.error);
                    ASSERT_TRUE(res2.data.get());
                    EXPECT_EQ(firstSize, res2.data->size());
                    loop.stop();
                });
        });

    loop.run();
}

// An archive whose header flags tile_compression=GZIP but whose individual tiles are stored
// without compression must be served without error (uncompressed bytes passed through as-is).
TEST(PMTilesFileSource, UncompressedTile) {
    util::RunLoop loop;

    PMTilesFileSource pmtiles(ResourceOptions::Default(), ClientOptions());

    std::unique_ptr<AsyncRequest> req = pmtiles.request(
        Resource::tile(toAbsoluteURL("uncompressed-tiles.pmtiles"), 1.0, 0, 0, 0, Tileset::Scheme::XYZ),
        [&](Response res) {
            req.reset();
            EXPECT_EQ(nullptr, res.error);
            ASSERT_TRUE(res.data.get());
            ASSERT_EQ(res.noContent, false);
            loop.stop();
        });

    loop.run();
}

// Tile requests that miss the directory cache together each load the directory they need. Storing
// a directory that is already cached must not give it a second place in the cache's eviction order:
// once more requests than the cache has entries (100) stored the same root directory, each further
// store evicted it at once, and the lookup that followed threw "map::at: key not found", failing the
// tile with "Error parsing PMTiles directory".
TEST(PMTilesFileSource, ConcurrentDirectoryLoads) {
    util::RunLoop loop;

    auto archive = std::make_shared<Archive>(makeLeafPerTileArchive(1));
    ScopedArchiveFileSource loader(FileSourceType::ResourceLoader, archive);
    PMTilesFileSource pmtiles(archiveResourceOptions(), ClientOptions());

    // Read the header, so the tile requests start at the root directory.
    ASSERT_EQ(nullptr, requestOnce(loop, pmtiles, {Resource::Source, archiveURL}).error);

    for (const auto &response : requestTogether(loop, pmtiles, *archive, std::vector<Resource>(150, tileResource(0)))) {
        expectTile(0, response);
    }

    // Both directories stayed cached, so another request fetches only the tile.
    const std::size_t fetches = archive->fetchCount();
    expectTile(0, requestOnce(loop, pmtiles, tileResource(0)));
    EXPECT_EQ(fetches + 1, archive->fetchCount());
}

// A directory that several requests loaded at once must stay usable while the cache evicts other
// directories. Each load used to queue the directory for eviction again; evicting the oldest copy
// removed it from the cache while the other copies stayed queued in front, so the next load of it
// was evicted as soon as it was stored and the tile failed with "map::at: key not found".
TEST(PMTilesFileSource, DirectoryCacheEviction) {
    util::RunLoop loop;

    constexpr uint64_t tileCount = 250;
    auto archive = std::make_shared<Archive>(makeLeafPerTileArchive(tileCount));
    ScopedArchiveFileSource loader(FileSourceType::ResourceLoader, archive);
    PMTilesFileSource pmtiles(archiveResourceOptions(), ClientOptions());

    // Read the header and the root directory.
    expectTile(0, requestOnce(loop, pmtiles, tileResource(0)));

    // Load tile 1's leaf directory with many requests at once.
    for (const auto &response : requestTogether(loop, pmtiles, *archive, std::vector<Resource>(50, tileResource(1)))) {
        expectTile(1, response);
    }

    // Pan across enough other tiles for the cache to start evicting, then come back to tile 1.
    for (uint64_t id = 2; id < 75; ++id) {
        expectTile(id, requestOnce(loop, pmtiles, tileResource(id)));
    }
    expectTile(1, requestOnce(loop, pmtiles, tileResource(1)));

    // Pan on, far past what the cache holds.
    for (uint64_t id = 75; id < tileCount; ++id) {
        expectTile(id, requestOnce(loop, pmtiles, tileResource(id)));
    }

    // Recently used directories stay cached: the root, which every lookup reads, and the last tile's
    // leaf, so the last tile again fetches only the tile.
    std::size_t fetches = archive->fetchCount();
    expectTile(tileCount - 1, requestOnce(loop, pmtiles, tileResource(tileCount - 1)));
    EXPECT_EQ(fetches + 1, archive->fetchCount());

    // An evicted directory is loaded again: tile 2's leaf, used long ago, is fetched before the tile.
    fetches = archive->fetchCount();
    expectTile(2, requestOnce(loop, pmtiles, tileResource(2)));
    EXPECT_EQ(fetches + 2, archive->fetchCount());
}

// Cancelling a request cancels the archive fetch it's waiting for, and its lookup goes no further.
TEST(PMTilesFileSource, CancelledRequestStopsLookup) {
    util::RunLoop loop;

    auto archive = std::make_shared<Archive>(makeLeafPerTileArchive(1));
    ScopedArchiveFileSource loader(FileSourceType::ResourceLoader, archive);
    PMTilesFileSource pmtiles(archiveResourceOptions(), ClientOptions());

    // Read the header, so the tile request's first fetch is the root directory.
    ASSERT_FALSE(requestOnce(loop, pmtiles, {Resource::Source, archiveURL}).error);

    archive->hold();
    std::unique_ptr<AsyncRequest> request = pmtiles.request(
        tileResource(0), [](const Response &) { ADD_FAILURE() << "the cancelled request was answered"; });
    ASSERT_TRUE(archive->waitForHeld(1));
    const std::size_t fetches = archive->fetchCount();

    request.reset();
    EXPECT_TRUE(archive->waitForOpenFetches(0));

    // Answering the cancelled fetch doesn't go on to the leaf directory. The request after it, which
    // the cache answers, waits for the file source to deal with the answer first.
    archive->release();
    ASSERT_FALSE(requestOnce(loop, pmtiles, {Resource::Source, archiveURL}).error);
    EXPECT_EQ(fetches, archive->fetchCount());
}

// Releasing an answered request frees the archive fetches it made.
TEST(PMTilesFileSource, FinishedRequestFreesFetches) {
    util::RunLoop loop;

    auto archive = std::make_shared<Archive>(makeLeafPerTileArchive(1));
    ScopedArchiveFileSource loader(FileSourceType::ResourceLoader, archive);
    PMTilesFileSource pmtiles(archiveResourceOptions(), ClientOptions());

    ASSERT_FALSE(requestOnce(loop, pmtiles, {Resource::Source, archiveURL}).error);
    expectTile(0, requestOnce(loop, pmtiles, tileResource(0)));
    EXPECT_TRUE(archive->waitForOpenFetches(0));
}

// For a remote archive, the cache is the ambient cache: a cache-only request reads the header,
// directories and tile from it and never from the network, and answers NotFound for what isn't
// there, so that the tile loader goes on to request the tile from the network.
TEST(PMTilesFileSource, CacheOnlyRemoteArchive) {
    util::RunLoop loop;

    auto network = std::make_shared<Archive>(makeLeafPerTileArchive(2));
    ScopedArchiveFileSource online(FileSourceType::Network, network);
    PMTilesFileSource pmtiles(archiveResourceOptions(), ClientOptions());

    const std::string url = "pmtiles://https://example.com/archive.pmtiles";
    constexpr auto cacheOnly = Resource::LoadingMethod::CacheOnly;

    Response response = requestOnce(loop, pmtiles, tileResource(0, url, cacheOnly));
    ASSERT_TRUE(response.error);
    EXPECT_EQ(Response::Error::Reason::NotFound, response.error->reason) << response.error->message;
    EXPECT_EQ(0u, network->fetchCount());

    // Loading the tile from the network stores what it fetched in the ambient cache...
    expectTile(0, requestOnce(loop, pmtiles, tileResource(0, url)));
    const std::size_t fetches = network->fetchCount();
    EXPECT_LT(0u, fetches);

    // ...where a cache-only request finds it.
    expectTile(0, requestOnce(loop, pmtiles, tileResource(0, url, cacheOnly)));

    // The other tile's leaf directory is in neither cache.
    response = requestOnce(loop, pmtiles, tileResource(1, url, cacheOnly));
    ASSERT_TRUE(response.error);
    EXPECT_EQ(Response::Error::Reason::NotFound, response.error->reason) << response.error->message;
    EXPECT_EQ(fetches, network->fetchCount());
}

// For a local archive, the file is the cache: a cache-only request reads the tile from it.
TEST(PMTilesFileSource, CacheOnlyLocalArchive) {
    util::RunLoop loop;

    auto network = std::make_shared<Archive>(makeLeafPerTileArchive(1));
    ScopedArchiveFileSource online(FileSourceType::Network, network);
    PMTilesFileSource pmtiles(archiveResourceOptions(), ClientOptions());

    const Response response = requestOnce(loop,
                                          pmtiles,
                                          Resource::tile(toAbsoluteURL("geography-class-png.pmtiles"),
                                                         1.0,
                                                         0,
                                                         0,
                                                         0,
                                                         Tileset::Scheme::XYZ,
                                                         Resource::LoadingMethod::CacheOnly));
    ASSERT_FALSE(response.error) << response.error->message;
    ASSERT_TRUE(response.data);
    EXPECT_FALSE(response.noContent);
    EXPECT_EQ(0u, network->fetchCount());
}
