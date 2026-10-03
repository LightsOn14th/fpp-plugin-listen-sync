/*
 * fpp-plugin-listen-sync: the part that runs inside fppd.
 *
 * This file is deliberately tiny. It does no networking beyond handing one UDP
 * datagram per event to 127.0.0.1, starts no threads, parses nothing, and
 * catches everything. All real work (relay WebSocket, clock sync, playlist
 * lookups, uploads) happens in listen_sync_daemon.py, a separate process, so a
 * failure there can never take fppd down.
 *
 * Datagrams (tab separated, one per message, UTF-8):
 *
 *   M <monotonic ns> <seconds> <media file>      position from MultiSync
 *   S <monotonic ns> <media file>                media stopped
 *   P <monotonic ns> <playlist name> <media>     playlist started (media may be empty)
 *
 * The monotonic clock is CLOCK_MONOTONIC, shared by every process on the host,
 * so the daemon can relate these stamps to its own clock exactly.
 */
#include <fpp-pch.h>

#include <arpa/inet.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "log.h"
#include "MultiSync.h"
#include "Plugin.h"
#include "Plugins.h"

namespace {

const char* PLUGIN_NAME = "fpp-plugin-listen-sync";
const uint16_t DAEMON_PORT = 39117;

// MultiSync reports the position every frame; the daemon needs far less.
const long long POSITION_INTERVAL_NS = 100 * 1000 * 1000LL;

long long monotonicNs() {
    struct timespec now;

    clock_gettime( CLOCK_MONOTONIC, &now );

    return (long long)now.tv_sec * 1000000000LL + now.tv_nsec;
}

/**
 * Copy `value` into `out`, replacing tabs and newlines so a field can never
 * break the datagram format.
 */
void appendField( std::string& out, const std::string& value ) {
    out += '\t';

    for ( char c : value ) {
        out += ( c == '\t' || c == '\n' || c == '\r' ) ? ' ' : c;
    }
}

} // namespace

class ListenSyncPlugin : public FPPPlugins::Plugin,
                         public FPPPlugins::PlaylistEventPlugin,
                         public MultiSyncPlugin {
public:
    ListenSyncPlugin() :
        FPPPlugins::Plugin( PLUGIN_NAME ) {
        try {
            int fd = socket( AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0 );

            memset( &daemon, 0, sizeof( daemon ) );
            daemon.sin_family = AF_INET;
            daemon.sin_port = htons( DAEMON_PORT );
            daemon.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
            sock.store( fd );

            MultiSync::INSTANCE.addMultiSyncPlugin( this );
            LogInfo( VB_PLUGIN, "ListenSync: sending positions to 127.0.0.1:%d\n", DAEMON_PORT );
        } catch ( ... ) {
            // Never let anything escape into fppd.
        }
    }

    virtual ~ListenSyncPlugin() {
        stop();
    }

    virtual std::function<bool()> shutdown() override {
        stop();

        return nullptr;
    }

    // ---- MultiSyncPlugin (fppd media threads) -------------------------------

    virtual void SendMediaSyncPacket( const std::string& filename, float seconds ) override {
        try {
            long long now = monotonicNs();
            bool changed = false;

            {
                std::lock_guard<std::mutex> lock( mutex );
                changed = filename != lastFile;

                if ( ! changed && now - lastPositionNs < POSITION_INTERVAL_NS ) {
                    return;
                }

                lastFile = filename;
                lastPositionNs = now;
            }

            char numbers[ 64 ];
            snprintf( numbers, sizeof( numbers ), "M\t%lld\t%.6f", now, (double)seconds );

            std::string message( numbers );
            appendField( message, filename );
            sendDatagram( message );
        } catch ( ... ) {
        }
    }

    virtual void SendMediaSyncStopPacket( const std::string& filename ) override {
        try {
            {
                std::lock_guard<std::mutex> lock( mutex );
                lastFile.clear();
                lastPositionNs = 0;
            }

            std::string message = "S\t" + std::to_string( monotonicNs() );
            appendField( message, filename );
            sendDatagram( message );
        } catch ( ... ) {
        }
    }

    // ---- PlaylistEventPlugin (fppd playlist thread) ------------------------

    virtual void playlistCallback( const Json::Value& playlist, const std::string& action,
                                   const std::string& section, int item ) override {
        try {
            if ( action != "start" && action != "playing" ) {
                return;
            }

            if ( ! playlist.isObject() ) {
                return;
            }

            const Json::Value* name = playlist.find( "name", "name" + 4 );

            if ( ! name || ! name->isString() || name->asString().empty() ) {
                return;
            }

            std::string media;
            const Json::Value* entry = playlist.find( "currentEntry", "currentEntry" + 12 );

            if ( entry && entry->isObject() ) {
                const Json::Value* mediaName = entry->find( "mediaName", "mediaName" + 9 );

                if ( mediaName && mediaName->isString() ) {
                    media = mediaName->asString();
                }
            }

            std::string message = "P\t" + std::to_string( monotonicNs() );
            appendField( message, name->asString() );
            appendField( message, media );
            sendDatagram( message );
        } catch ( ... ) {
        }
    }

private:
    void sendDatagram( const std::string& message ) {
        int fd = sock.load();

        if ( fd < 0 || message.size() > 4096 ) {
            return;
        }

        // Non-blocking; if the daemon is not running the datagram is dropped.
        sendto( fd, message.data(), message.size(), MSG_DONTWAIT | MSG_NOSIGNAL,
                (const struct sockaddr*)&daemon, sizeof( daemon ) );
    }

    void stop() {
        try {
            if ( stopped.exchange( true ) ) {
                return;
            }

            MultiSync::INSTANCE.removeMultiSyncPlugin( this );

            int fd = sock.exchange( -1 );

            if ( fd >= 0 ) {
                close( fd );
            }
        } catch ( ... ) {
        }
    }

    std::mutex mutex;
    std::string lastFile;
    long long lastPositionNs = 0;
    std::atomic<int> sock { -1 };
    std::atomic<bool> stopped { false };
    struct sockaddr_in daemon;
};

extern "C" {
FPPPlugins::Plugin* createPlugin() {
    return new ListenSyncPlugin();
}
}
