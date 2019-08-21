#include "HttpEndpoint.hpp"

#include "Asset.hpp"
#include "AssetDatabase.hpp"
#include "Constants.hpp"
#include "schemas/FlatAsset_generated.h"
#include "schemas/FlatAssetData_generated.h"
#include "schemas/FlatList_generated.h"

#include "glog/logging.h"
#include "libbase64.h"
#include "pistache/endpoint.h"
#include "pistache/peer.h"
#include "pistache/router.h"

#include <chrono>
#include <mutex>
#include <deque>
#include <unordered_map>

namespace {

inline uint32_t packIpv4(const std::string& ip) {
    uint32_t packedIp = 0;
    const char* startChar = ip.data();
    char* endChar = nullptr;
    while (startChar - ip.data() < ip.size()) {
        int digit = strtol(startChar, &endChar, 10);
        packedIp = (packedIp << 8) | static_cast<uint8_t>(digit);
        startChar = endChar + 1;
    }

    return packedIp;
}

inline std::string unpackIpv4(uint32_t ip) {
    std::array<char, 24> buffer;
    snprintf(buffer.data(), 24, "%u.%u.%u.%u", (ip >> 24) & 0xff, (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff);
    return std::string(buffer.data());
}

}  // namespace

namespace Confab {

/*! Handler class for processing incoming HTTP requests. Uses the Pistache Router to connect specific REST-style API
 * queries from clients to private method calls within this class.
 */
class HttpEndpoint::HttpHandler {
public:
    /*! Constructs an empty handler.
     *
     * \param listenPort The TCP port to listen on for HTTP requests.
     * \param numThreads The number of threads to use to listen on the port.
     * \param assetDatabase A pointer to the shared AssetDatabase instance.
     */
    HttpHandler(int listenPort, int numThreads, std::shared_ptr<AssetDatabase> assetDatabase) :
        m_listenPort(listenPort),
        m_numThreads(numThreads),
        m_assetDatabase(assetDatabase) { }

    /*! Setup HTTP URL routes and initialize server.
     */
    void setupRoutes() {
        Pistache::Address address(Pistache::Ipv4::any(), Pistache::Port(m_listenPort));
        m_server.reset(new Pistache::Http::Endpoint(address));
        auto opts = Pistache::Http::Endpoint::options().threads(m_numThreads).maxRequestSize(2 * kPageSize);
        m_server->init(opts);

        Pistache::Rest::Routes::Get(m_router, "/asset/id/:key", Pistache::Rest::Routes::bind(
            &HttpEndpoint::HttpHandler::getAsset, this));
        Pistache::Rest::Routes::Post(m_router, "/asset/id/:key", Pistache::Rest::Routes::bind(
            &HttpEndpoint::HttpHandler::postAsset, this));

        Pistache::Rest::Routes::Get(m_router, "/asset/name", Pistache::Rest::Routes::bind(
            &HttpEndpoint::HttpHandler::getNamedAsset, this));

        Pistache::Rest::Routes::Get(m_router, "/asset/data/:key/:chunk", Pistache::Rest::Routes::bind(
            &HttpEndpoint::HttpHandler::getAssetData, this));
        Pistache::Rest::Routes::Post(m_router, "/asset/data/:key/:chunk", Pistache::Rest::Routes::bind(
            &HttpEndpoint::HttpHandler::postAssetData, this));

        Pistache::Rest::Routes::Get(m_router, "/list/id/:key", Pistache::Rest::Routes::bind(
            &HttpEndpoint::HttpHandler::getList, this));
        Pistache::Rest::Routes::Post(m_router, "/list/id/:key", Pistache::Rest::Routes::bind(
            &HttpEndpoint::HttpHandler::postList, this));

        Pistache::Rest::Routes::Get(m_router, "/list/name", Pistache::Rest::Routes::bind(
            &HttpEndpoint::HttpHandler::getNamedList, this));

        Pistache::Rest::Routes::Get(m_router, "/list/items/:key/:from", Pistache::Rest::Routes::bind(
            &HttpEndpoint::HttpHandler::getListItems, this));

        Pistache::Rest::Routes::Get(m_router, "/state", Pistache::Rest::Routes::bind(
            &HttpEndpoint::HttpHandler::getState, this));
        Pistache::Rest::Routes::Post(m_router, "/state", Pistache::Rest::Routes::bind(
            &HttpEndpoint::HttpHandler::postState, this));

    }

    /*! Starts a thread that will listen on the provided TCP port and process incoming requests for storage and
     * retrieval of assets.
     */
    void startServerThread() {
        m_server->setHandler(m_router.handler());
        m_server->serveThreaded();
    }

    /*! Starts the server on this thread, blocking the thread. Call one of this method or startServerThread().
     */
    void startServer() {
        m_server->setHandler(m_router.handler());
        m_server->serve();
    }

    /*! Stops serving threads, closes ports.
     */
    void shutdown() {
        m_server->shutdown();
    }

private:
    void getAsset(const Pistache::Rest::Request& request, Pistache::Http::ResponseWriter response) {
        auto keyString = request.param(":key").as<std::string>();
        LOG(INFO) << "processing HTTP GET request for /asset/id/" << keyString;
        uint64_t key = Asset::stringToKey(keyString);
        RecordPtr record = m_assetDatabase->findAsset(key);
        if (record->empty()) {
            LOG(ERROR) << "HTTP get request for Asset " << keyString << " not found, returning 404.";
            response.headers().add<Pistache::Http::Header::Server>("confab");
            response.send(Pistache::Http::Code::Not_Found);
        } else {
            LOG(INFO) << "HTTP get request returning Asset data for " << keyString;
            char base64[kPageSize];
            size_t encodedSize = 0;
            base64_encode(reinterpret_cast<const char*>(record->data().data()), record->data().size(), base64,
                &encodedSize, 0);
            if (encodedSize >= kPageSize) {
                LOG(ERROR) << "encoded size: " << encodedSize << " exceeds buffer size " << kPageSize;
            }
            response.headers().add<Pistache::Http::Header::Server>("confab");
            response.send(Pistache::Http::Code::Ok, std::string(base64, encodedSize), MIME(Text, Plain));
        }
    }

    void postAsset(const Pistache::Rest::Request& request, Pistache::Http::ResponseWriter response) {
        auto keyString = request.param(":key").as<std::string>();
        uint64_t key = Asset::stringToKey(keyString);
        uint8_t decoded[kPageSize];
        size_t decodedSize;
        base64_decode(request.body().data(), request.body().size(), reinterpret_cast<char*>(decoded), &decodedSize, 0);
        SizedPointer postedData(decoded, decodedSize);
        LOG(INFO) << "processing HTTP POST request for /asset/id/" << keyString << ", " << postedData.size() << " bytes.";

        // Sanity-check the provided serialized FlatAsset data.
        auto verifier = flatbuffers::Verifier(postedData.data(), postedData.size());
        bool status = Data::VerifyFlatAssetBuffer(verifier);
        if (status) {
            LOG(INFO) << "verified FlatAsset " << keyString;
            status = m_assetDatabase->storeAsset(key, postedData);
        } else {
            LOG(ERROR) << "posted data did not verify for asset " << keyString;
        }

        response.headers().add<Pistache::Http::Header::Server>("confab");
        if (status) {
            LOG(INFO) << "sending OK response after storing asset " << keyString;
            response.send(Pistache::Http::Code::Ok);
        } else {
            LOG(ERROR) << "sending error response after failure to store asset " << keyString;
            response.send(Pistache::Http::Code::Internal_Server_Error);
        }
    }

    void getNamedAsset(const Pistache::Rest::Request& request, Pistache::Http::ResponseWriter response) {
        auto name = request.body();
        LOG(INFO) << "processing HTTP GET request for /asset/name/" << name;
        RecordPtr record = m_assetDatabase->findNamedAsset(name);
        if (record->empty()) {
            LOG(ERROR) << "HTTP get request for named Asset " << name << " not found, returning 404.";
            response.headers().add<Pistache::Http::Header::Server>("confab");
            response.send(Pistache::Http::Code::Not_Found);
        } else {
            LOG(INFO) << "HTTP get request returning named asset data for " << name;
            char base64[kPageSize];
            size_t encodedSize = 0;
            base64_encode(reinterpret_cast<const char*>(record->data().data()), record->data().size(), base64,
                &encodedSize, 0);
            if (encodedSize >= kPageSize) {
                LOG(ERROR) << "encoded size: " << encodedSize << " exceeds buffer size " << kPageSize;
            }
            response.headers().add<Pistache::Http::Header::Server>("confab");
            response.send(Pistache::Http::Code::Ok, std::string(base64, encodedSize), MIME(Text, Plain));
        }
    }

    void getAssetData(const Pistache::Rest::Request& request, Pistache::Http::ResponseWriter response) {
        auto keyString = request.param(":key").as<std::string>();
        auto chunk = request.param(":chunk").as<uint64_t>();
        LOG(INFO) << "processing HTTP GET request for /asset/data/" << keyString << "/" << chunk;
        uint64_t key = Asset::stringToKey(keyString);
        RecordPtr assetData = m_assetDatabase->loadAssetDataChunk(key, chunk);
        response.headers().add<Pistache::Http::Header::Server>("confab");
        if (assetData->empty()) {
            LOG(ERROR) << "HTTP get request for Asset Data " << keyString << " chunk " << chunk
                << " not found, returning 404.";
            response.send(Pistache::Http::Code::Not_Found);
        } else {
            LOG(INFO) << "HTTP get request for Asset Data " << keyString << " chunk " << chunk
                << " returning Asset Data.";
            char base64[kPageSize];
            size_t encodedSize = 0;
            base64_encode(reinterpret_cast<const char*>(assetData->data().data()), assetData->data().size(), base64,
                &encodedSize, 0);
            if (encodedSize >= kPageSize) {
                LOG(ERROR) << "encoded Size: " << encodedSize << " exceeds buffer size " << kPageSize;
            } else {
                LOG(INFO) << "sending " << encodedSize << " bytes of Asset Data.";
            }
            response.send(Pistache::Http::Code::Ok, std::string(base64, encodedSize), MIME(Text, Plain));
        }
    }

    void postAssetData(const Pistache::Rest::Request& request, Pistache::Http::ResponseWriter response) {
        auto keyString = request.param(":key").as<std::string>();
        auto chunk = request.param(":chunk").as<uint64_t>();
        LOG(INFO) << "processing HTTP POST request for /asset/data/" << keyString << "/" << chunk;
        uint64_t key = Asset::stringToKey(keyString);
        uint8_t decoded[kPageSize];
        size_t decodedSize;
        base64_decode(request.body().data(), request.body().size(), reinterpret_cast<char*>(decoded), &decodedSize, 0);
        auto verifier = flatbuffers::Verifier(decoded, decodedSize);
        bool status = Data::VerifyFlatAssetDataBuffer(verifier);
        if (status) {
            LOG(INFO) << "verified FlatAssetData " << keyString << " chunk " << chunk;
            SizedPointer postedData(decoded, decodedSize);
            status = m_assetDatabase->storeAssetDataChunk(key, chunk, postedData);
        } else {
            LOG(ERROR) << "posted data did not verify for asset data " << keyString << " chunk " << chunk;
        }
        response.headers().add<Pistache::Http::Header::Server>("confab");
        if (status) {
            LOG(INFO) << "sending OK response after storing asset " << keyString << " data chunk " << chunk;
            response.send(Pistache::Http::Code::Ok);
        } else {
            LOG(ERROR) << "sending error response after failure to store asset " << keyString << " data chunk "
                << chunk;
            response.send(Pistache::Http::Code::Internal_Server_Error);
        }
    }

    void getList(const Pistache::Rest::Request& request, Pistache::Http::ResponseWriter response) {
        auto keyString = request.param(":key").as<std::string>();
        LOG(INFO) << "processing GET request for /list/id " << keyString;
        uint64_t key = Asset::stringToKey(keyString);
        RecordPtr listData = m_assetDatabase->loadList(key);
        response.headers().add<Pistache::Http::Header::Server>("confab");
        if (listData->empty()) {
            LOG(ERROR) << "get frequest for list " << keyString << " not found, 404.";
            response.send(Pistache::Http::Code::Not_Found);
        } else {
            LOG(INFO) << "get request for list " << keyString << " returning list data.";
            char base64[kPageSize];
            size_t encodedSize = 0;
            base64_encode(reinterpret_cast<const char*>(listData->data().data()), listData->data().size(), base64,
                &encodedSize, 0);
            if (encodedSize >= kPageSize) {
                LOG(ERROR) << "encoded size: " << encodedSize << " exceeds buffer size " << kPageSize;
            } else {
                LOG(INFO) << "sending " << encodedSize << " bytes of List data.";
            }
            response.send(Pistache::Http::Code::Ok, std::string(base64, encodedSize), MIME(Text, Plain));
        }
    }

    void postList(const Pistache::Rest::Request& request, Pistache::Http::ResponseWriter response) {
        auto keyString = request.param(":key").as<std::string>();
        LOG(INFO) << "processing POST request for /list/id " << keyString;
        uint64_t key = Asset::stringToKey(keyString);
        uint8_t decoded[kPageSize];
        size_t decodedSize;
        base64_decode(request.body().data(), request.body().size(), reinterpret_cast<char*>(decoded), &decodedSize, 0);
        auto verifier = flatbuffers::Verifier(decoded, decodedSize);
        bool status = Data::VerifyFlatListBuffer(verifier);
        if (status) {
            LOG(INFO) << "verified FlatList " << keyString;
            SizedPointer postedData(decoded, decodedSize);
            status = m_assetDatabase->storeList(key, postedData);
        } else {
            LOG(ERROR) << "posted data did not verify for list " << keyString;
        }
        response.headers().add<Pistache::Http::Header::Server>("confab");
        if (status) {
            LOG(INFO) << "sending OK response after storing list " << keyString;
            response.send(Pistache::Http::Code::Ok);
        } else {
            LOG(ERROR) << "sending error response after failure to store list " << keyString;
            response.send(Pistache::Http::Code::Internal_Server_Error);
        }
    }

    void getNamedList(const Pistache::Rest::Request& request, Pistache::Http::ResponseWriter response) {
        auto name = request.body();
        LOG(INFO) << "processing GET request for /list/name '" << name << "'.";
        RecordPtr listData = m_assetDatabase->findNamedList(name);
        response.headers().add<Pistache::Http::Header::Server>("confab");
        if (listData->empty()) {
            LOG(ERROR) << "get request for list named " << name << " not found, 404.";
            response.send(Pistache::Http::Code::Not_Found);
        } else {
            char base64[kPageSize];
            size_t encodedSize = 0;
            base64_encode(reinterpret_cast<const char*>(listData->data().data()), listData->data().size(), base64,
                &encodedSize, 0);
            if (encodedSize >= kPageSize) {
                LOG(ERROR) << "encoded Size: " << encodedSize << " exceeds buffer size " << kPageSize;
            } else {
                LOG(INFO) << "sending " << encodedSize << " bytes of Asset Data.";
            }
            response.send(Pistache::Http::Code::Ok, std::string(base64, encodedSize), MIME(Text, Plain));
        }
    }

    void getListItems(const Pistache::Rest::Request& request, Pistache::Http::ResponseWriter response) {
        auto keyString = request.param(":key").as<std::string>();
        auto fromString = request.param(":from").as<std::string>();
        LOG(INFO) << "processing get /list/items/" << keyString << "/" << fromString;

        uint64_t key = Asset::stringToKey(keyString);
        uint64_t token = Asset::stringToKey(fromString);
        std::array<uint64_t, kPageSize / 17> pairs;
        size_t numPairs = m_assetDatabase->getListNext(key, token, pairs.size() / 2, pairs.data());
        response.headers().add<Pistache::Http::Header::Server>("confab");
        if (numPairs == 0) {
            LOG(ERROR) << "error retrieving iterator pair list for " << keyString;
            response.send(Pistache::Http::Code::Internal_Server_Error);
        } else {
            LOG(INFO) << "sending " << numPairs << " tokens back to client on list " << keyString;
            std::string pairList;
            for (auto i = 0; i < numPairs; ++i) {
                pairList += Asset::keyToString(pairs[i * 2]) + " " + Asset::keyToString(pairs[(i * 2) + 1]) + "\n";
            }
            response.send(Pistache::Http::Code::Ok, pairList, MIME(Text, Plain));
        }
    }

    void getState(const Pistache::Rest::Request& request, Pistache::Http::ResponseWriter response) {
        LOG(INFO) << "processing get /state";
        // The state response is encoded as a series of <address>\t<status string>\n string pairs.
        std::string statePairs;
        size_t numPairs = 0;
        auto now = std::chrono::steady_clock::now();
        auto dropTime = now - std::chrono::milliseconds((3 * kStatusUpdatePeriodMs) / 2);
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            // First groom the state queue to only recent updates.
            auto oldest = m_stateQueue.size() ? m_stateQueue.front().first : now;
            while (oldest < dropTime) {
                m_stateQueue.pop_front();
                oldest = m_stateQueue.size() ? m_stateQueue.front().first : now;
            }
            // It is likely there are duplicates in the state update queue, so we de-dupe them.
            std::unordered_set<uint32_t> current;
            for (auto pair : m_stateQueue) {
                current.insert(pair.second);
            }
            std::vector<uint32_t> outdated;
            // We now iterate through the map, to groom old entries from it as well.
            for (auto pair : m_stateMap) {
                // If in current we append the address and serialized state to our return string.
                if (current.count(pair.first)) {
                    statePairs += unpackIpv4(pair.first) + "\t" + pair.second + "\n";
                    ++numPairs;
                } else {
                    outdated.push_back(pair.first);
                }
            }
            // Lastly drop all of the outdated data from the map, to keep it lean.
            for (auto old : outdated) {
                LOG(INFO) << "dropping stale ip " << unpackIpv4(old) << " from status map.";
                m_stateMap.erase(old);
            }
        }
        LOG(INFO) << "returning " << numPairs << " states";
        response.headers().add<Pistache::Http::Header::Server>("confab");
        response.send(Pistache::Http::Code::Ok, statePairs, MIME(Text, Plain));
    }

    // Note this function doesn't log due to excessive logspam.
    void postState(const Pistache::Rest::Request& request, Pistache::Http::ResponseWriter response) {
        uint32_t address = packIpv4(request.address().host());
        if (address == 0) {
            LOG(WARNING) << "state update error parsing address " << request.address().host();
            response.headers().add<Pistache::Http::Header::Server>("confab");
            response.send(Pistache::Http::Code::Internal_Server_Error);
        } else {
            // Drop everything older than 1.5 times as long as our update period.
            auto now = std::chrono::steady_clock::now();
            auto dropTime = now - std::chrono::milliseconds((3 * kStatusUpdatePeriodMs) / 2);
            {
                std::lock_guard<std::mutex> lock(m_stateMutex);
                auto oldest = m_stateQueue.size() ? m_stateQueue.front().first : now;
                while (oldest < dropTime) {
                    m_stateQueue.pop_front();
                    oldest = m_stateQueue.size() ? m_stateQueue.front().first : now;
                }
                m_stateQueue.push_back(std::make_pair(now, address));
                m_stateMap.insert_or_assign(address, request.body());
            }

            response.headers().add<Pistache::Http::Header::Server>("confab");
            response.send(Pistache::Http::Code::Ok);
        }
    }

    int m_listenPort;
    int m_numThreads;
    std::shared_ptr<AssetDatabase> m_assetDatabase;
    std::shared_ptr<Pistache::Http::Endpoint> m_server;
    Pistache::Rest::Router m_router;

    std::mutex m_stateMutex;
    using TimeName = std::pair<std::chrono::steady_clock::time_point, uint32_t>;
    std::deque<TimeName> m_stateQueue;
    std::unordered_map<uint32_t, std::string> m_stateMap;
};

HttpEndpoint::HttpEndpoint(int listenPort, int numThreads, std::shared_ptr<AssetDatabase> assetDatabase) :
    m_handler(new HttpHandler(listenPort, numThreads, assetDatabase)) {
}

HttpEndpoint::~HttpEndpoint() {
}

void HttpEndpoint::startServerThread() {
    m_handler->setupRoutes();
    m_handler->startServerThread();
}

void HttpEndpoint::startServer() {
    m_handler->setupRoutes();
    m_handler->startServer();
}

void HttpEndpoint::shutdown() {
    m_handler->shutdown();
}

}  // namespace Confab

