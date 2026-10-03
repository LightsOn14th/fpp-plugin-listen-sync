/*
 * fpp-plugin-listen-sync
 *
 * Sends FPP's playback position to a Listen Sync relay so phones and browsers
 * can play the show audio in sync with the lights, and uploads the show's
 * audio files to the relay so listeners can download them ahead of time.
 *
 * Position comes from FPP's MultiSync media callbacks, which FPP 10 corrects
 * for audio output latency, so the position reflects what is leaving the
 * speakers. Every sample is stamped with a monotonic clock and converted to
 * the relay clock using an NTP-style offset measured over the same WebSocket,
 * so the Pi's wall clock does not need to be accurate.
 *
 * All network work runs on the plugin's own event loop thread and a worker
 * thread, never on fppd's media or main threads.
 */
#include <fpp-pch.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

#include <drogon/HttpClient.h>
#include <drogon/WebSocketClient.h>
#include <drogon/utils/Utilities.h>
#include <trantor/net/EventLoopThread.h>

#include "common.h"
#include "fpphttp.h"
#include "log.h"
#include "MultiSync.h"
#include "Plugin.h"
#include "Plugins.h"
#include "settings.h"

namespace {

const std::string PLUGIN_NAME = "fpp-plugin-listen-sync";
const std::string API_PATH = "/ListenSync";
const std::string DEFAULT_RELAY = "https://listen.lightson14th.com";
const std::string LOCAL_API = "http://127.0.0.1";

const double SAMPLE_INTERVAL_S = 0.5;
const double PING_INTERVAL_S = 2.0;
const double HEARTBEAT_INTERVAL_S = 10.0;
const double UPLOAD_INTERVAL_S = 15 * 60.0;
const double HTTP_TIMEOUT_S = 120.0;
const int PING_BURST = 5;
const size_t CLOCK_SAMPLES = 16;
const size_t CLOCK_BEST_OF = 3;

const std::set<std::string> AUDIO_EXTENSIONS = { "aac", "flac", "m4a", "mp3", "ogg", "opus", "wav" };

/**
 * Monotonic time in milliseconds with sub-millisecond precision.
 */
double steadyMs() {
    using namespace std::chrono;

    return duration<double, std::milli>( steady_clock::now().time_since_epoch() ).count();
}

std::string trim( const std::string& value ) {
    const char* space = " \t\r\n";
    size_t start = value.find_first_not_of( space );

    if ( start == std::string::npos ) {
        return "";
    }

    size_t end = value.find_last_not_of( space );

    return value.substr( start, end - start + 1 );
}

// ---- type-safe JSON reads ---------------------------------------------------
//
// jsoncpp throws Json::LogicError when a value is read as the wrong type (for
// example isMember() or operator[] on an array, or asLargestInt() on a string).
// An uncaught exception on a plugin thread aborts all of fppd, so every read of
// JSON this plugin did not build itself goes through these helpers.

/**
 * The member `key` of `object`, or a null value when `object` is not an object.
 */
const Json::Value& jsonMember( const Json::Value& object, const char* key ) {
    static const Json::Value null;

    if ( ! object.isObject() ) {
        return null;
    }

    const Json::Value* found = object.find( key, key + strlen( key ) );

    return found ? *found : null;
}

/**
 * A string member, or `fallback` when missing or not a string.
 */
std::string jsonString( const Json::Value& object, const char* key, const std::string& fallback = "" ) {
    const Json::Value& value = jsonMember( object, key );

    return value.isString() ? value.asString() : fallback;
}

/**
 * A number member that may also arrive as a numeric string (FPP sends
 * `sizeBytes` as a string on 64-bit systems), or `fallback`.
 */
long long jsonInteger( const Json::Value& object, const char* key, long long fallback = -1 ) {
    const Json::Value& value = jsonMember( object, key );

    if ( value.isIntegral() ) {
        return value.asLargestInt();
    }

    if ( value.isDouble() ) {
        return (long long)value.asDouble();
    }

    if ( value.isString() ) {
        const std::string text = value.asString();
        char* end = nullptr;
        long long parsed = strtoll( text.c_str(), &end, 10 );

        return end && end != text.c_str() && *end == '\0' ? parsed : fallback;
    }

    return fallback;
}

/**
 * A numeric member as a double, or `fallback`.
 */
double jsonNumber( const Json::Value& object, const char* key, double fallback ) {
    const Json::Value& value = jsonMember( object, key );

    return value.isNumeric() ? value.asDouble() : fallback;
}

/**
 * Run `fn`, logging instead of propagating any exception. Used around every
 * piece of plugin code that runs on a thread or callback fppd does not own.
 */
void guarded( const char* where, const std::function<void()>& fn ) {
    try {
        fn();
    } catch ( const std::exception& error ) {
        LogErr( VB_PLUGIN, "ListenSync: error in %s: %s\n", where, error.what() );
    } catch ( ... ) {
        LogErr( VB_PLUGIN, "ListenSync: unknown error in %s\n", where );
    }
}

std::string lowerExtension( const std::string& file ) {
    size_t dot = file.find_last_of( '.' );

    if ( dot == std::string::npos ) {
        return "";
    }

    std::string extension = file.substr( dot + 1 );
    std::transform( extension.begin(), extension.end(), extension.begin(), ::tolower );

    return extension;
}

std::string contentTypeFor( const std::string& file ) {
    std::string extension = lowerExtension( file );

    if ( extension == "mp3" ) {
        return "audio/mpeg";
    }

    if ( extension == "m4a" ) {
        return "audio/mp4";
    }

    if ( extension == "wav" ) {
        return "audio/wav";
    }

    if ( extension == "ogg" || extension == "opus" ) {
        return "audio/ogg";
    }

    if ( extension == "flac" ) {
        return "audio/flac";
    }

    if ( extension == "aac" ) {
        return "audio/aac";
    }

    return "application/octet-stream";
}

/**
 * Split a relay URL such as `https://listen.example.com` into the host string
 * drogon clients take. Returns an empty string for an unusable URL.
 */
std::string relayHost( const std::string& url ) {
    std::string value = trim( url );

    while ( ! value.empty() && value.back() == '/' ) {
        value.pop_back();
    }

    if ( value.rfind( "https://", 0 ) != 0 && value.rfind( "http://", 0 ) != 0 ) {
        return "";
    }

    size_t schemeEnd = value.find( "://" ) + 3;

    if ( value.find( '/', schemeEnd ) != std::string::npos || value.size() <= schemeEnd ) {
        return "";
    }

    return value;
}

/**
 * Percent-encode a URL path segment or query value (RFC 3986). drogon's
 * urlEncodeComponent() uses form encoding, which turns spaces into `+`; FPP
 * and the relay decode paths strictly, so `+` would stay a literal plus.
 */
std::string percentEncode( const std::string& value ) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;

    for ( unsigned char c : value ) {
        if ( isalnum( c ) || c == '-' || c == '_' || c == '.' || c == '~' ) {
            out += (char)c;
        } else {
            out += '%';
            out += hex[ c >> 4 ];
            out += hex[ c & 0x0F ];
        }
    }

    return out;
}

/**
 * The WebSocket form of a relay host string: drogon's WebSocket client only
 * recognises `ws://` and `wss://`.
 */
std::string webSocketHost( const std::string& host ) {
    if ( host.rfind( "https://", 0 ) == 0 ) {
        return "wss://" + host.substr( 8 );
    }

    if ( host.rfind( "http://", 0 ) == 0 ) {
        return "ws://" + host.substr( 7 );
    }

    return host;
}

std::string reqResultName( drogon::ReqResult result ) {
    switch ( result ) {
        case drogon::ReqResult::Ok:
            return "ok";
        case drogon::ReqResult::BadResponse:
            return "bad response";
        case drogon::ReqResult::NetworkFailure:
            return "network failure";
        case drogon::ReqResult::BadServerAddress:
            return "bad server address";
        case drogon::ReqResult::Timeout:
            return "timeout";
        case drogon::ReqResult::HandshakeError:
            return "TLS handshake error";
        case drogon::ReqResult::InvalidCertificate:
            return "invalid certificate";
        default:
            return "request failed";
    }
}

/**
 * NTP-style offset between this machine's monotonic clock and the relay clock.
 * Same algorithm as the browser client: median offset of the fastest few
 * recent round trips.
 */
class ClockSync {
public:
    void add( double t0, double ts, double t1 ) {
        double rtt = t1 - t0;

        if ( rtt < 0 ) {
            return;
        }

        samples.push_back( { ts - ( t0 + t1 ) / 2.0, rtt } );

        if ( samples.size() > CLOCK_SAMPLES ) {
            samples.erase( samples.begin() );
        }
    }

    bool ready() const {
        return ! samples.empty();
    }

    double offset() const {
        std::vector<Sample> best = fastest();

        if ( best.empty() ) {
            return 0;
        }

        std::vector<double> offsets;

        for ( const Sample& sample : best ) {
            offsets.push_back( sample.offset );
        }

        std::sort( offsets.begin(), offsets.end() );

        return offsets[ offsets.size() / 2 ];
    }

    double rtt() const {
        std::vector<Sample> best = fastest();

        return best.empty() ? -1 : best.front().rtt;
    }

    void reset() {
        samples.clear();
    }

private:
    struct Sample {
        double offset;
        double rtt;
    };

    std::vector<Sample> fastest() const {
        std::vector<Sample> sorted = samples;

        std::sort( sorted.begin(), sorted.end(), []( const Sample& a, const Sample& b ) {
            return a.rtt < b.rtt;
        } );

        if ( sorted.size() > CLOCK_BEST_OF ) {
            sorted.resize( CLOCK_BEST_OF );
        }

        return sorted;
    }

    std::vector<Sample> samples;
};

} // namespace

class ListenSyncPlugin : public FPPPlugins::Plugin,
                         public FPPPlugins::PlaylistEventPlugin,
                         public FPPPlugins::APIProviderPlugin,
                         public MultiSyncPlugin {
public:
    ListenSyncPlugin() :
        FPPPlugins::Plugin( PLUGIN_NAME, true ) {
        readSettings();
        tokenPath = FPP_DIR_MEDIA( "/plugindata/" + PLUGIN_NAME + "/token" );
        token = readToken();

        loopThread = std::make_unique<trantor::EventLoopThread>( "ListenSync" );
        loopThread->run();
        loop = loopThread->getLoop();

        worker = std::thread( [this]() {
            workerMain();
        } );

        MultiSync::INSTANCE.addMultiSyncPlugin( this );

        queue( [this]() {
            startTimers();
            connect();
        } );

        LogInfo( VB_PLUGIN, "ListenSync: started, relay %s\n", relayUrl.c_str() );
    }

    virtual ~ListenSyncPlugin() {
        stopEverything();
    }

    // ---- FPPPlugins::Plugin ------------------------------------------------

    virtual std::function<bool()> shutdown() override {
        stopEverything();

        // drogon may still have socket callbacks queued for a moment after the
        // clients stop; give them time to drain before FPP destroys the object.
        long long until = GetTimeMS() + 3000;

        return [until]() {
            return GetTimeMS() >= until;
        };
    }

    virtual void settingChanged( const std::string& key, const std::string& value ) override {
        bool reconnect = false;

        {
            std::lock_guard<std::mutex> lock( mutex );
            std::string oldRelay = relayUrl;
            bool oldEnabled = enabled;

            readSettingsLocked();
            reconnect = oldRelay != relayUrl || oldEnabled != enabled;
        }

        if ( reconnect ) {
            restartConnection();
        }
    }

    // ---- MultiSyncPlugin (called on fppd media threads) -------------------

    virtual void SendMediaSyncPacket( const std::string& filename, float seconds ) override {
        double now = steadyMs();
        bool changed = false;

        {
            std::lock_guard<std::mutex> lock( mutex );
            changed = ! playing || filename != currentFile;
            playing = true;
            currentFile = filename;
            currentPos = seconds;
            sampleStamp = now;
        }

        if ( changed ) {
            queue( [this]() {
                sendSample();
            } );
        }
    }

    virtual void SendMediaSyncStopPacket( const std::string& filename ) override {
        {
            std::lock_guard<std::mutex> lock( mutex );

            if ( ! playing ) {
                return;
            }

            playing = false;
        }

        queue( [this, filename]() {
            Json::Value message;
            message[ "type" ] = "media";
            message[ "state" ] = "stopped";
            message[ "file" ] = filename;
            message[ "pos" ] = 0;
            message[ "at" ] = 0;
            send( message );
        } );
    }

    // ---- PlaylistEventPlugin ----------------------------------------------

    virtual void playlistCallback( const Json::Value& playlist, const std::string& action,
                                   const std::string& section, int item ) override {
        // Runs on an fppd playlist thread: never let anything escape.
        guarded( "playlistCallback", [&]() {
            if ( action != "start" && action != "playing" ) {
                return;
            }

            std::string name = jsonString( playlist, "name" );

            if ( name.empty() ) {
                return;
            }

            // The media of the entry that is starting, used when the playlist
            // itself cannot be read (e.g. an on-the-fly playlist FPP generates
            // when a sequence is played directly).
            std::string media = jsonString( jsonMember( playlist, "currentEntry" ), "mediaName" );

            {
                std::lock_guard<std::mutex> lock( mutex );

                if ( name == playlistName ) {
                    return;
                }

                playlistName = name;
                playlistRequested = name;
                playlistFallbackMedia = media;
            }

            workerCv.notify_all();
        } );
    }

    // ---- APIProviderPlugin ------------------------------------------------

    virtual void registerApis() override {
        FPPPlugins::registerPluginApi(
            API_PATH,
            [this]( const HttpRequestPtr& req, HttpCallback&& callback ) {
                handleApi( req, std::move( callback ) );
            },
            { drogon::Get, drogon::Post }, true );
    }

    virtual void unregisterApis() override {
        FPPPlugins::unregisterPluginApi( API_PATH );
    }

private:
    // ---- settings and token -------------------------------------------------

    void readSettings() {
        std::lock_guard<std::mutex> lock( mutex );
        readSettingsLocked();
    }

    void readSettingsLocked() {
        auto get = [this]( const std::string& key, const std::string& fallback ) {
            auto found = settings.find( key );

            return found == settings.end() || found->second.empty() ? fallback : found->second;
        };

        enabled = get( "ListenSyncEnabled", "1" ) == "1";
        uploadMedia = get( "ListenSyncUploadMedia", "1" ) == "1";
        relayUrl = trim( get( "ListenSyncRelayURL", DEFAULT_RELAY ) );
    }

    std::string readToken() {
        std::ifstream in( tokenPath );
        std::string value;

        if ( in ) {
            std::getline( in, value );
        }

        return trim( value );
    }

    bool writeToken( const std::string& value ) {
        std::string dir = FPP_DIR_MEDIA( "/plugindata/" + PLUGIN_NAME );

        if ( mkdir( FPP_DIR_MEDIA( "/plugindata" ).c_str(), 0755 ) != 0 && errno != EEXIST ) {
            return false;
        }

        if ( mkdir( dir.c_str(), 0700 ) != 0 && errno != EEXIST ) {
            return false;
        }

        std::ofstream out( tokenPath, std::ios::trunc );

        if ( ! out ) {
            return false;
        }

        out << value << "\n";
        out.close();
        chmod( tokenPath.c_str(), 0600 );

        return true;
    }

    // ---- event loop helpers -------------------------------------------------

    void queue( std::function<void()> fn ) {
        if ( loop && ! stopping ) {
            loop->queueInLoop( [fn = std::move( fn )]() {
                guarded( "event loop task", fn );
            } );
        }
    }

    void every( double seconds, const char* where, std::function<void()> fn ) {
        timers.push_back( loop->runEvery( seconds, [where, fn = std::move( fn )]() {
            guarded( where, fn );
        } ) );
    }

    void startTimers() {
        every( SAMPLE_INTERVAL_S, "sample timer", [this]() {
            sendSample();
        } );
        every( PING_INTERVAL_S, "ping timer", [this]() {
            sendPing();
        } );
        every( HEARTBEAT_INTERVAL_S, "heartbeat timer", [this]() {
            Json::Value message;
            message[ "type" ] = "hb";
            send( message );
        } );
        every( UPLOAD_INTERVAL_S, "upload timer", [this]() {
            requestUpload();
        } );
    }

    void stopEverything() {
        if ( stopping.exchange( true ) ) {
            return;
        }

        MultiSync::INSTANCE.removeMultiSyncPlugin( this );

        {
            std::lock_guard<std::mutex> lock( mutex );
            workerStop = true;
        }

        workerCv.notify_all();

        if ( worker.joinable() ) {
            worker.join();
        }

        if ( loop ) {
            // Tear down the clients on their own loop and wait for that to
            // finish, then stop the loop. The loop thread's destructor joins
            // it, so no handler of ours can run after this returns.
            auto tornDown = std::make_shared<std::promise<void>>();
            std::future<void> done = tornDown->get_future();

            loop->runInLoop( [this, tornDown]() {
                guarded( "shutdown", [this]() {
                    for ( auto id : timers ) {
                        loop->invalidateTimer( id );
                    }

                    timers.clear();
                    closeSocket();
                } );
                tornDown->set_value();
            } );

            done.wait_for( std::chrono::seconds( 5 ) );
        }

        loopThread.reset();
        loop = nullptr;

        LogInfo( VB_PLUGIN, "ListenSync: stopped\n" );
    }

    // ---- relay connection (loop thread) -------------------------------------

    void connect() {
        if ( stopping ) {
            return;
        }

        std::string host;
        std::string currentToken;

        {
            std::lock_guard<std::mutex> lock( mutex );

            if ( ! enabled ) {
                connectionState = "disabled";

                return;
            }

            if ( token.empty() ) {
                connectionState = "no token";

                return;
            }

            host = relayHost( relayUrl );
            currentToken = token;

            if ( host.empty() ) {
                connectionState = "invalid relay URL";

                return;
            }

            connectionState = "connecting";
        }

        auto client = drogon::WebSocketClient::newWebSocketClient( webSocketHost( host ), loop );
        auto request = drogon::HttpRequest::newHttpRequest();
        drogon::WebSocketClient* raw = client.get();

        request->setPath( "/fpp" );
        request->addHeader( "Authorization", "Bearer " + currentToken );

        client->setMessageHandler( [this, raw]( const std::string& message, const drogon::WebSocketClientPtr&,
                                                const drogon::WebSocketMessageType& type ) {
            guarded( "relay message", [&]() {
                if ( socket.get() == raw && type == drogon::WebSocketMessageType::Text ) {
                    onMessage( message );
                }
            } );
        } );

        client->setConnectionClosedHandler( [this, raw]( const drogon::WebSocketClientPtr& ) {
            guarded( "relay close", [&]() {
                if ( socket.get() == raw ) {
                    onClosed( "connection closed" );
                }
            } );
        } );

        socket = client;

        client->connectToServer( request, [this, raw]( drogon::ReqResult result, const drogon::HttpResponsePtr& response,
                                                       const drogon::WebSocketClientPtr& ) {
            guarded( "relay connect", [&]() {
                if ( socket.get() != raw ) {
                    return;
                }

                if ( result == drogon::ReqResult::Ok ) {
                    onOpen();

                    return;
                }

                std::string reason = reqResultName( result );

                if ( response ) {
                    reason += " (HTTP " + std::to_string( (int)response->statusCode() ) + ")";
                }

                onClosed( reason );
            } );
        } );
    }

    void onOpen() {
        {
            std::lock_guard<std::mutex> lock( mutex );
            connected = true;
            connectionState = "connected";
            lastError.clear();
            reconnectAttempt = 0;
            clock.reset();
        }

        LogInfo( VB_PLUGIN, "ListenSync: connected to relay\n" );

        for ( int i = 0; i < PING_BURST; i++ ) {
            loop->runAfter( 0.1 * i, [this]() {
                guarded( "ping", [this]() {
                    sendPing();
                } );
            } );
        }

        sendPlaylist();
        requestUpload();
    }

    void onClosed( const std::string& reason ) {
        int attempt = 0;

        {
            std::lock_guard<std::mutex> lock( mutex );
            connected = false;
            connectionState = "reconnecting";
            lastError = reason;
            attempt = reconnectAttempt++;
        }

        socket.reset();
        LogWarn( VB_PLUGIN, "ListenSync: relay connection lost: %s\n", reason.c_str() );

        if ( stopping || reconnectPending ) {
            return;
        }

        reconnectPending = true;
        double delay = std::min( 30.0, (double)( 1 << std::min( attempt, 5 ) ) );

        loop->runAfter( delay, [this]() {
            guarded( "reconnect", [this]() {
                reconnectPending = false;

                if ( ! socket ) {
                    connect();
                }
            } );
        } );
    }

    void closeSocket() {
        if ( socket ) {
            auto old = socket;
            socket.reset();
            old->stop();
        }

        std::lock_guard<std::mutex> lock( mutex );
        connected = false;
    }

    void restartConnection() {
        queue( [this]() {
            closeSocket();
            connect();
        } );
    }

    void onMessage( const std::string& text ) {
        double received = steadyMs();
        Json::Value message;

        if ( ! LoadJsonFromString( text, message ) || ! message.isObject() ) {
            return;
        }

        if ( jsonString( message, "type" ) != "pong" ) {
            return;
        }

        double t0 = jsonNumber( message, "t0", -1 );
        double ts = jsonNumber( message, "ts", -1 );

        if ( t0 < 0 || ts < 0 ) {
            return;
        }

        std::lock_guard<std::mutex> lock( mutex );
        clock.add( t0, ts, received );
    }

    void send( const Json::Value& message ) {
        if ( ! socket ) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock( mutex );

            if ( ! connected ) {
                return;
            }
        }

        auto connection = socket->getConnection();

        if ( connection && connection->connected() ) {
            connection->send( SaveJsonToString( message ) );
        }
    }

    void sendPing() {
        Json::Value message;
        message[ "type" ] = "ping";
        message[ "id" ] = ++pingId;
        message[ "t0" ] = steadyMs();
        send( message );
    }

    void sendSample() {
        Json::Value message;

        {
            std::lock_guard<std::mutex> lock( mutex );

            if ( ! playing || ! connected || ! clock.ready() ) {
                return;
            }

            message[ "type" ] = "media";
            message[ "state" ] = "playing";
            message[ "file" ] = currentFile;
            message[ "pos" ] = currentPos;
            message[ "at" ] = sampleStamp + clock.offset();
        }

        send( message );
    }

    void sendPlaylist() {
        Json::Value message;

        {
            std::lock_guard<std::mutex> lock( mutex );

            if ( playlistName.empty() ) {
                return;
            }

            message[ "type" ] = "playlist";
            message[ "name" ] = playlistName;
            message[ "items" ] = Json::Value( Json::arrayValue );

            for ( const std::string& item : playlistItems ) {
                message[ "items" ].append( item );
            }
        }

        send( message );
    }

    // ---- worker thread: playlist lookups and media uploads ------------------

    void requestUpload() {
        {
            std::lock_guard<std::mutex> lock( mutex );
            uploadRequested = true;
        }

        workerCv.notify_all();
    }

    void workerMain() {
        while ( true ) {
            std::string playlist;
            std::string fallbackMedia;
            bool upload = false;

            {
                std::unique_lock<std::mutex> lock( mutex );
                workerCv.wait( lock, [this]() {
                    return workerStop || uploadRequested || ! playlistRequested.empty();
                } );

                if ( workerStop ) {
                    return;
                }

                playlist.swap( playlistRequested );
                fallbackMedia = playlistFallbackMedia;
                upload = uploadRequested;
                uploadRequested = false;
            }

            bool active = false;

            {
                std::lock_guard<std::mutex> lock( mutex );
                active = enabled;
            }

            if ( ! active ) {
                continue;
            }

            if ( ! playlist.empty() ) {
                guarded( "fetchPlaylist", [&]() {
                    fetchPlaylist( playlist, fallbackMedia );
                } );
            }

            if ( upload ) {
                guarded( "uploadMissingMedia", [&]() {
                    uploadMissingMedia();
                } );
            }
        }
    }

    drogon::HttpClientPtr newClient( const std::string& host ) {
        // The client runs on the plugin loop; the synchronous sendRequest()
        // calls below block only this worker thread.
        return drogon::HttpClient::newHttpClient( host, loop );
    }

    void fetchPlaylist( const std::string& name, const std::string& fallbackMedia ) {
        std::vector<std::string> items = readPlaylistMedia( name );

        if ( items.empty() && ! fallbackMedia.empty() ) {
            items.push_back( fallbackMedia );
        }

        {
            std::lock_guard<std::mutex> lock( mutex );

            if ( name != playlistName ) {
                return;
            }

            playlistItems = items;
        }

        queue( [this]() {
            sendPlaylist();
        } );
    }

    /**
     * Media names in a saved playlist, or an empty list when the playlist
     * cannot be read or is not in the expected shape.
     */
    std::vector<std::string> readPlaylistMedia( const std::string& name ) {
        std::vector<std::string> items;

        // On-the-fly playlists ("Song.fseq/Song.mp3") are not saved files.
        if ( name.find( '/' ) != std::string::npos ) {
            return items;
        }

        auto client = newClient( LOCAL_API );
        auto request = drogon::HttpRequest::newHttpRequest();

        request->setPathEncode( false );
        request->setPath( "/api/playlist/" + percentEncode( name ) );

        auto [ result, response ] = client->sendRequest( request, HTTP_TIMEOUT_S );

        if ( result != drogon::ReqResult::Ok || ! response || response->statusCode() != drogon::k200OK ) {
            LogWarn( VB_PLUGIN, "ListenSync: could not read playlist %s\n", name.c_str() );

            return items;
        }

        Json::Value root;

        if ( ! LoadJsonFromString( std::string( response->body() ), root ) || ! root.isObject() ) {
            LogWarn( VB_PLUGIN, "ListenSync: playlist %s is not a JSON object\n", name.c_str() );

            return items;
        }

        for ( const char* section : { "leadIn", "mainPlaylist", "leadOut" } ) {
            const Json::Value& entries = jsonMember( root, section );

            if ( ! entries.isArray() ) {
                continue;
            }

            for ( const Json::Value& entry : entries ) {
                std::string media = jsonString( entry, "mediaName" );

                if ( ! media.empty() ) {
                    items.push_back( media );
                }
            }
        }

        return items;
    }

    void setUploadStatus( const std::string& state, int done, int total, const std::string& error = "" ) {
        std::lock_guard<std::mutex> lock( mutex );
        uploadState = state;
        uploadDone = done;
        uploadTotal = total;

        if ( ! error.empty() ) {
            uploadError = error;
        }
    }

    void uploadMissingMedia() {
        std::string host;
        std::string currentToken;

        {
            std::lock_guard<std::mutex> lock( mutex );

            if ( ! uploadMedia || ! enabled || token.empty() ) {
                return;
            }

            host = relayHost( relayUrl );
            currentToken = token;
            uploadError.clear();
        }

        if ( host.empty() ) {
            return;
        }

        setUploadStatus( "checking", 0, 0 );

        auto local = newClient( LOCAL_API );
        auto relay = newClient( host );

        // What FPP has.
        auto listRequest = drogon::HttpRequest::newHttpRequest();
        listRequest->setPath( "/api/files/Music" );
        auto [ listResult, listResponse ] = local->sendRequest( listRequest, HTTP_TIMEOUT_S );
        Json::Value localFiles;

        if ( listResult != drogon::ReqResult::Ok || ! listResponse ||
             ! LoadJsonFromString( std::string( listResponse->body() ), localFiles ) ) {
            setUploadStatus( "error", 0, 0, "could not list FPP music files" );

            return;
        }

        // What the relay has.
        auto manifestRequest = drogon::HttpRequest::newHttpRequest();
        manifestRequest->setPath( "/api/media" );
        manifestRequest->addHeader( "Authorization", "Bearer " + currentToken );
        auto [ manifestResult, manifestResponse ] = relay->sendRequest( manifestRequest, HTTP_TIMEOUT_S );
        Json::Value manifest;

        if ( manifestResult != drogon::ReqResult::Ok || ! manifestResponse ||
             manifestResponse->statusCode() != drogon::k200OK ||
             ! LoadJsonFromString( std::string( manifestResponse->body() ), manifest ) ) {
            setUploadStatus( "error", 0, 0, "could not read the relay media list (" + reqResultName( manifestResult ) + ")" );

            return;
        }

        std::vector<std::pair<std::string, std::string>> pending;

        const Json::Value& files = jsonMember( localFiles, "files" );
        const Json::Value& uploaded = jsonMember( manifest, "files" );

        if ( ! files.isArray() ) {
            setUploadStatus( "error", 0, 0, "unexpected FPP music file list" );

            return;
        }

        for ( const Json::Value& file : files ) {
            std::string name = jsonString( file, "name" );
            long long size = jsonInteger( file, "sizeBytes" );

            if ( name.empty() || size < 0 || name.find( '/' ) != std::string::npos ||
                 AUDIO_EXTENSIONS.count( lowerExtension( name ) ) == 0 ) {
                continue;
            }

            std::string version = std::to_string( size ) + "-" + jsonString( file, "mtime" );

            if ( jsonString( uploaded, name.c_str() ) != version ) {
                pending.emplace_back( name, version );
            }
        }

        int total = (int)pending.size();
        int done = 0;

        setUploadStatus( total > 0 ? "uploading" : "up to date", 0, total );

        for ( const auto& [ name, version ] : pending ) {
            {
                std::lock_guard<std::mutex> lock( mutex );

                if ( workerStop ) {
                    return;
                }
            }

            auto getRequest = drogon::HttpRequest::newHttpRequest();
            getRequest->setPathEncode( false );
            getRequest->setPath( "/api/file/Music/" + percentEncode( name ) );
            auto [ getResult, getResponse ] = local->sendRequest( getRequest, HTTP_TIMEOUT_S );

            if ( getResult != drogon::ReqResult::Ok || ! getResponse || getResponse->statusCode() != drogon::k200OK ) {
                setUploadStatus( "uploading", done, total, "could not read " + name + " from FPP" );
                continue;
            }

            auto putRequest = drogon::HttpRequest::newHttpRequest();
            putRequest->setMethod( drogon::Put );
            putRequest->setPathEncode( false );
            putRequest->setPath( "/media/" + percentEncode( name ) + "?version=" +
                                 percentEncode( version ) );
            putRequest->addHeader( "Authorization", "Bearer " + currentToken );
            putRequest->setContentTypeString( contentTypeFor( name ) );
            putRequest->setBody( std::string( getResponse->body() ) );

            auto [ putResult, putResponse ] = relay->sendRequest( putRequest, HTTP_TIMEOUT_S );

            if ( putResult != drogon::ReqResult::Ok || ! putResponse || putResponse->statusCode() != drogon::k200OK ) {
                std::string status = putResponse ? std::to_string( (int)putResponse->statusCode() ) : reqResultName( putResult );

                setUploadStatus( "uploading", done, total, "upload of " + name + " failed (" + status + ")" );
                LogWarn( VB_PLUGIN, "ListenSync: upload of %s failed (%s)\n", name.c_str(), status.c_str() );
                continue;
            }

            done++;
            setUploadStatus( "uploading", done, total );
            LogInfo( VB_PLUGIN, "ListenSync: uploaded %s\n", name.c_str() );
        }

        {
            std::lock_guard<std::mutex> lock( mutex );
            uploadState = done == total ? "up to date" : "incomplete";
            lastUploadRun = time( nullptr );
        }
    }

    // ---- HTTP API (drogon request threads) ----------------------------------

    void handleApi( const HttpRequestPtr& req, HttpCallback&& callback ) {
        try {
            routeApi( req, std::move( callback ) );
        } catch ( const std::exception& error ) {
            LogErr( VB_PLUGIN, "ListenSync: error in API handler: %s\n", error.what() );
        }
    }

    void routeApi( const HttpRequestPtr& req, HttpCallback&& callback ) {
        std::string path = req->path();
        std::string action = path.size() > API_PATH.size() ? path.substr( path.find_last_of( '/' ) + 1 ) : "status";

        if ( req->method() == drogon::Post && action == "token" ) {
            std::string value = trim( std::string( req->body() ) );

            if ( value.empty() || value.size() > 512 || ! writeToken( value ) ) {
                callback( makeStringResponse( "{\"error\":\"could not save token\"}", 400, "application/json" ) );

                return;
            }

            {
                std::lock_guard<std::mutex> lock( mutex );
                token = value;
            }

            restartConnection();
            callback( makeStringResponse( SaveJsonToString( statusJson() ), 200, "application/json" ) );

            return;
        }

        if ( req->method() == drogon::Post && action == "upload" ) {
            requestUpload();
            callback( makeStringResponse( SaveJsonToString( statusJson() ), 200, "application/json" ) );

            return;
        }

        if ( req->method() == drogon::Get && ( action == "status" || action == "ListenSync" ) ) {
            callback( makeStringResponse( SaveJsonToString( statusJson() ), 200, "application/json" ) );

            return;
        }

        callback( makeStringResponse( "{\"error\":\"not found\"}", 404, "application/json" ) );
    }

    Json::Value statusJson() {
        std::lock_guard<std::mutex> lock( mutex );
        Json::Value status;

        status[ "enabled" ] = enabled;
        status[ "relayUrl" ] = relayUrl;
        status[ "tokenSet" ] = ! token.empty();
        status[ "connection" ] = connectionState;
        status[ "lastError" ] = lastError;
        status[ "multiSyncEnabled" ] = multiSync != nullptr && multiSync->isMultiSyncEnabled();
        status[ "clockReady" ] = clock.ready();
        status[ "clockOffsetMs" ] = clock.ready() ? clock.offset() : Json::Value();
        status[ "rttMs" ] = clock.ready() ? clock.rtt() : Json::Value();
        status[ "playing" ] = playing;
        status[ "file" ] = playing ? Json::Value( currentFile ) : Json::Value();
        status[ "position" ] = playing ? Json::Value( currentPos ) : Json::Value();
        status[ "playlist" ] = playlistName;
        status[ "playlistItems" ] = (int)playlistItems.size();
        status[ "uploadMedia" ] = uploadMedia;
        status[ "upload" ][ "state" ] = uploadState;
        status[ "upload" ][ "done" ] = uploadDone;
        status[ "upload" ][ "total" ] = uploadTotal;
        status[ "upload" ][ "error" ] = uploadError;
        status[ "upload" ][ "lastRun" ] = (Json::Int64)lastUploadRun;

        return status;
    }

    // ---- state --------------------------------------------------------------

    std::mutex mutex;

    // Settings and token.
    bool enabled = true;
    bool uploadMedia = true;
    std::string relayUrl = DEFAULT_RELAY;
    std::string token;
    std::string tokenPath;

    // Latest position from MultiSync.
    bool playing = false;
    std::string currentFile;
    double currentPos = 0;
    double sampleStamp = 0;

    // Playlist.
    std::string playlistName;
    std::vector<std::string> playlistItems;

    // Connection (socket itself is only touched on the loop thread).
    drogon::WebSocketClientPtr socket;
    bool connected = false;
    bool reconnectPending = false;
    int reconnectAttempt = 0;
    int pingId = 0;
    std::string connectionState = "starting";
    std::string lastError;
    ClockSync clock;

    // Uploads.
    std::string uploadState = "idle";
    int uploadDone = 0;
    int uploadTotal = 0;
    std::string uploadError;
    time_t lastUploadRun = 0;

    // Threads.
    std::unique_ptr<trantor::EventLoopThread> loopThread;
    trantor::EventLoop* loop = nullptr;
    std::vector<trantor::TimerId> timers;
    std::thread worker;
    std::condition_variable workerCv;
    bool workerStop = false;
    bool uploadRequested = false;
    std::string playlistRequested;
    std::string playlistFallbackMedia;
    std::atomic<bool> stopping { false };
};

extern "C" {
FPPPlugins::Plugin* createPlugin() {
    return new ListenSyncPlugin();
}
}
