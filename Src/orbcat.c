/* SPDX-License-Identifier: BSD-3-Clause */

/*
 * ITM Catter for Orbuculum
 * ========================
 *
 */

#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#ifdef WIN32
    #include <winsock2.h>
#else
    #include <sys/ioctl.h>
    #include <netinet/in.h>
    #include <netdb.h>
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <inttypes.h>
#ifdef WIN32
#else
    #include <termios.h>
#endif

#include "nw.h"
#include "git_version_info.h"
#include "generics.h"
#include "tpiuDecoder.h"
#include "itmDecoder.h"
#include "msgDecoder.h"

#ifdef WIN32
    // https://stackoverflow.com/a/14388707/995351
    #define SO_REUSEPORT SO_REUSEADDR 
#endif

#define NUM_CHANNELS  32
#define HW_CHANNEL    (NUM_CHANNELS)      /* Make the hardware fifo on the end of the software ones */

#define MAX_STRING_LENGTH (100)           /* Maximum length that will be output from a fifo for a single event */

// Record for options, either defaults or from command line
struct
{
    /* Config information */
    bool useTPIU;
    uint32_t tpiuChannel;
    bool forceITMSync;
    uint32_t hwOutputs;

    /* Sink information */
    char *presFormat[NUM_CHANNELS + 1];

    /* Source information */
    int port;
    char *server;

    char *file;                                          /* File host connection */
    bool endTerminate;                                  /* Terminate when file/socket "ends" */

} options = {.forceITMSync = true, .tpiuChannel = 1, .port = NWCLIENT_SERVER_PORT, .server = "localhost"};

struct
{
    /* The decoders and the packets from them */
    struct ITMDecoder i;
    struct ITMPacket h;
    struct TPIUDecoder t;
    struct TPIUPacket p;
    enum timeDelay timeStatus;           /* Indicator of if this time is exact */
    uint64_t timeStamp;                  /* Latest received time */
} _r;
// ====================================================================================================
// ====================================================================================================
// ====================================================================================================
// Handler for individual message types from SWO
// ====================================================================================================
// ====================================================================================================
// ====================================================================================================
void _handleException( struct excMsg *m, struct ITMDecoder *i )

{
    assert( m->msgtype == MSG_EXCEPTION );

    if ( !( options.hwOutputs & ( 1 << HWEVENT_EXCEPTION ) ) )
    {
        return;
    }

    const char *exNames[] = {"Thread", "Reset", "NMI", "HardFault", "MemManage", "BusFault", "UsageFault", "UNKNOWN_7",
                             "UNKNOWN_8", "UNKNOWN_9", "UNKNOWN_10", "SVCall", "Debug Monitor", "UNKNOWN_13", "PendSV", "SysTick"
                            };
    const char *exEvent[] = {"Enter", "Exit", "Resume"};
    fprintf( stdout, "%d,%s,%s" EOL, HWEVENT_EXCEPTION, exEvent[m->eventType], exNames[m->exceptionNumber] );
}
// ====================================================================================================
void _handleDWTEvent( struct dwtMsg *m, struct ITMDecoder *i )

{
    assert( m->msgtype == MSG_DWT_EVENT );

    if ( !( options.hwOutputs & ( 1 << HWEVENT_DWT ) ) )
    {
        return;
    }

    const char *evName[] = {"CPI", "Exc", "Sleep", "LSU", "Fold", "Cyc"};

    for ( uint32_t i = 0; i < 6; i++ )
    {
        if ( m->event & ( 1 << i ) )
        {
            fprintf( stdout, "%d,%s" EOL, HWEVENT_DWT, evName[m->event] );
        }
    }
}
// ====================================================================================================
void _handlePCSample( struct pcSampleMsg *m, struct ITMDecoder *i )

{
    assert( m->msgtype == MSG_PC_SAMPLE );

    if ( !( options.hwOutputs & ( 1 << HWEVENT_PCSample ) ) )
    {
        return;
    }

    fprintf( stdout, "%d,0x%08x" EOL, HWEVENT_PCSample, m->pc );
}
// ====================================================================================================
void _handleDataRWWP( struct watchMsg *m, struct ITMDecoder *i )

{
    assert( m->msgtype == MSG_DATA_RWWP );

    if ( !( options.hwOutputs & ( 1 << HWEVENT_RWWT ) ) )
    {
        return;
    }

    fprintf( stdout, "%d,%d,%s,0x%x" EOL, HWEVENT_RWWT, m->comp, m->isWrite ? "Write" : "Read", m->data );
}
// ====================================================================================================
void _handleDataAccessWP( struct wptMsg *m, struct ITMDecoder *i )


{
    assert( m->msgtype == MSG_DATA_ACCESS_WP );

    if ( !( options.hwOutputs & ( 1 << HWEVENT_AWP ) ) )
    {
        return;
    }

    fprintf( stdout, "%d,%d,0x%08x" EOL, HWEVENT_AWP, m->comp, m->data );
}
// ====================================================================================================
void _handleDataOffsetWP(  struct oswMsg *m, struct ITMDecoder *i )

{
    assert( m->msgtype == MSG_OSW );

    if ( !( options.hwOutputs & ( 1 << HWEVENT_OFS ) ) )
    {
        return;
    }

    fprintf( stdout, "%d,%d,0x%04x" EOL, HWEVENT_OFS, m->comp, m->offset );
}
// ====================================================================================================
void _handleSW( struct swMsg *m, struct ITMDecoder *i )

{
    assert( m->msgtype == MSG_SOFTWARE );

    if ( ( m->srcAddr < NUM_CHANNELS ) && ( options.presFormat[m->srcAddr] ) )
    {
        // formatted output....start with specials
        if ( strstr( options.presFormat[m->srcAddr], "%f" ) )
        {
            /* type punning on same host, after correctly building 32bit val
             * only unsafe on systems where u32/float have diff byte order */
            float *nastycast = ( float * )&m->value;
            fprintf( stdout, options.presFormat[m->srcAddr], *nastycast, *nastycast, *nastycast, *nastycast );
        }
        else if ( strstr( options.presFormat[m->srcAddr], "%c" ) )
        {
            /* Format contains %c, so execute repeatedly for all characters in sent data */
            uint8_t op[4] = {m->value & 0xff, ( m->value >> 8 ) & 0xff, ( m->value >> 16 ) & 0xff, ( m->value >> 24 ) & 0xff};
            uint32_t l = 0;

            do
            {
                fprintf( stdout, options.presFormat[m->srcAddr], op[l], op[l], op[l] );
            }
            while ( ++l < m->len );
        }
        else
        {
            fprintf( stdout, options.presFormat[m->srcAddr], m->value, m->value, m->value, m->value );
        }
    }
}
// ====================================================================================================
void _handleTS( struct TSMsg *m, struct ITMDecoder *i )

{
    assert( m->msgtype == MSG_TS );

    if ( !( options.hwOutputs & ( 1 << HWEVENT_TS ) ) )
    {
        return;
    }

    _r.timeStamp += m->timeInc;

    fprintf( stdout, "%d,%d,%" PRIu64 EOL, HWEVENT_TS, _r.timeStatus, _r.timeStamp );
}
// ====================================================================================================
void _itmPumpProcess( char c )

{
    struct msg decoded;

    typedef void ( *handlers )( void *decoded, struct ITMDecoder * i );

    /* Handlers for each complete message received */
    static const handlers h[MSG_NUM_MSGS] =
    {
        /* MSG_UNKNOWN */         NULL,
        /* MSG_RESERVED */        NULL,
        /* MSG_ERROR */           NULL,
        /* MSG_NONE */            NULL,
        /* MSG_SOFTWARE */        ( handlers )_handleSW,
        /* MSG_NISYNC */          NULL,
        /* MSG_OSW */             ( handlers )_handleDataOffsetWP,
        /* MSG_DATA_ACCESS_WP */  ( handlers )_handleDataAccessWP,
        /* MSG_DATA_RWWP */       ( handlers )_handleDataRWWP,
        /* MSG_PC_SAMPLE */       ( handlers )_handlePCSample,
        /* MSG_DWT_EVENT */       ( handlers )_handleDWTEvent,
        /* MSG_EXCEPTION */       ( handlers )_handleException,
        /* MSG_TS */              ( handlers )_handleTS
    };

    switch ( ITMPump( &_r.i, c ) )
    {
        case ITM_EV_NONE:
            break;

        case ITM_EV_UNSYNCED:
            genericsReport( V_INFO, "ITM Unsynced" EOL );
            break;

        case ITM_EV_SYNCED:
            genericsReport( V_INFO, "ITM Synced" EOL );
            break;

        case ITM_EV_OVERFLOW:
            genericsReport( V_WARN, "ITM Overflow" EOL );
            break;

        case ITM_EV_ERROR:
            genericsReport( V_WARN, "ITM Error" EOL );
            break;

        case ITM_EV_PACKET_RXED:
            ITMGetDecodedPacket( &_r.i, &decoded );

            /* See if we decoded a dispatchable match. genericMsg is just used to access */
            /* the first two members of the decoded structs in a portable way.           */
            if ( h[decoded.genericMsg.msgtype] )
            {
                ( h[decoded.genericMsg.msgtype] )( &decoded, &_r.i );
            }

            break;

        default:
            break;
    }
}
// ====================================================================================================
// ====================================================================================================
// ====================================================================================================
// Protocol pump for decoding messages
// ====================================================================================================
// ====================================================================================================
// ====================================================================================================
void _protocolPump( uint8_t c )

{
    if ( options.useTPIU )
    {
        switch ( TPIUPump( &_r.t, c ) )
        {
            case TPIU_EV_NEWSYNC:
            case TPIU_EV_SYNCED:
                ITMDecoderForceSync( &_r.i, true );
                break;

            case TPIU_EV_RXING:
            case TPIU_EV_NONE:
                break;

            case TPIU_EV_UNSYNCED:
                ITMDecoderForceSync( &_r.i, false );
                break;

            case TPIU_EV_RXEDPACKET:
                if ( !TPIUGetPacket( &_r.t, &_r.p ) )
                {
                    genericsReport( V_WARN, "TPIUGetPacket fell over" EOL );
                }

                for ( uint32_t g = 0; g < _r.p.len; g++ )
                {
                    if ( _r.p.packet[g].s == options.tpiuChannel )
                    {
                        _itmPumpProcess( _r.p.packet[g].d );
                        continue;
                    }

                    if  ( _r.p.packet[g].s != 0 )
                    {
                        genericsReport( V_INFO, "Unknown TPIU channel %02x" EOL, _r.p.packet[g].s );
                    }
                }

                break;

            case TPIU_EV_ERROR:
                genericsReport( V_WARN, "****ERROR****" EOL );
                break;
        }
    }
    else
    {
        _itmPumpProcess( c );
    }
}
// ====================================================================================================
void _printHelp( char *progName )

{
    fprintf( stdout, "Usage: %s [options]" EOL, progName );
    fprintf( stdout, "      -c: <Number>,<Format> of channel to add into output stream (repeat per channel)" EOL );
    fprintf( stdout, "      -e: Terminate when the file/socket ends/is closed, or attempt to wait for more / reconnect" EOL );
    fprintf( stdout, "      -f: <filename> Take input from specified file" EOL );
    fprintf( stdout, "      -h: This help" EOL );
    fprintf( stdout, "      -n: Enforce sync requirement for ITM (i.e. ITM needsd to issue syncs)" EOL );
    fprintf( stdout, "      -s: <Server>:<Port> to use" EOL );
    fprintf( stdout, "      -t <channel>: Use TPIU decoder on specified channel (normally 1)" EOL );
    fprintf( stdout, "      -v: <level> Verbose mode 0(errors)..3(debug)" EOL );
}
// ====================================================================================================
int _processOptions( int argc, char *argv[] )

{
    int c;
    char *chanConfig;
    unsigned int chan;
    char *chanIndex;
#define DELIMITER ','

    while ( ( c = getopt ( argc, argv, "c:ef:hns:t:v:" ) ) != -1 )
        switch ( c )
        {
            // ------------------------------------
            case 'h':
                _printHelp( argv[0] );
                return false;

            // ------------------------------------
            case 'e':
                options.endTerminate = true;
                break;

            // ------------------------------------
            case 'f':
                options.file = optarg;
                break;

            // ------------------------------------
            case 'n':
                options.forceITMSync = false;
                break;

            // ------------------------------------
            case 's':
                options.server = optarg;

                // See if we have an optional port number too
                char *a = optarg;

                while ( ( *a ) && ( *a != ':' ) )
                {
                    a++;
                }

                if ( *a == ':' )
                {
                    *a = 0;
                    options.port = atoi( ++a );
                }

                if ( !options.port )
                {
                    options.port = NWCLIENT_SERVER_PORT;
                }

                break;

            // ------------------------------------
            case 't':
                options.useTPIU = true;
                options.tpiuChannel = atoi( optarg );
                break;

            // ------------------------------------
            case 'v':
                genericsSetReportLevel( atoi( optarg ) );
                break;

            // ------------------------------------
            /* Individual channel setup */
            case 'c':
                chanIndex = chanConfig = strdup( optarg );
                chan = atoi( optarg );

                if ( chan >= NUM_CHANNELS )
                {
                    genericsReport( V_ERROR, "Channel index out of range" EOL );
                    return false;
                }

                /* Scan for format */
                while ( ( *chanIndex ) && ( *chanIndex != DELIMITER ) )
                {
                    chanIndex++;
                }

                if ( !*chanIndex )
                {
                    genericsReport( V_ERROR, "No output format for channel %d" EOL, chan );
                    return false;
                }

                *chanIndex++ = 0;
                options.presFormat[chan] = strdup( genericsUnescape( chanIndex ) );
                break;

            // ------------------------------------
            case '?':
                if ( optopt == 'b' )
                {
                    genericsReport( V_ERROR, "Option '%c' requires an argument." EOL, optopt );
                }
                else if ( !isprint ( optopt ) )
                {
                    genericsReport( V_ERROR, "Unknown option character `\\x%x'." EOL, optopt );
                }

                return false;

            // ------------------------------------
            default:
                return false;
                // ------------------------------------
        }

    if ( ( options.useTPIU ) && ( !options.tpiuChannel ) )
    {
        genericsReport( V_ERROR, "TPIU set for use but no channel set for ITM output" EOL );
        return false;
    }

    genericsReport( V_INFO, "orbcat V" VERSION " (Git %08X %s, Built " BUILD_DATE EOL, GIT_HASH, ( GIT_DIRTY ? "Dirty" : "Clean" ) );

    genericsReport( V_INFO, "Server     : %s:%d" EOL, options.server, options.port );
    genericsReport( V_INFO, "ForceSync  : %s" EOL, options.forceITMSync ? "true" : "false" );

    if ( options.file )
    {

        genericsReport( V_INFO, "Input File : %s", options.file );

        if ( options.endTerminate )
        {
            genericsReport( V_INFO, " (Terminate on exhaustion)" EOL );
        }
        else
        {
            genericsReport( V_INFO, " (Ongoing read)" EOL );
        }
    }

    if ( options.useTPIU )
    {
        genericsReport( V_INFO, "Using TPIU : true (ITM on channel %d)" EOL, options.tpiuChannel );
    }
    else
    {
        genericsReport( V_INFO, "Using TPIU : false" EOL );
    }

    genericsReport( V_INFO, "Channels   :" EOL );

    for ( int g = 0; g < NUM_CHANNELS; g++ )
    {
        if ( options.presFormat[g] )
        {
            genericsReport( V_INFO, "             %02d [%s]" EOL, g, genericsEscape( options.presFormat[g] ) );
        }
    }

    return true;
}
// ====================================================================================================

int fileFeeder( void )

{
    int f;
    unsigned char cbw[TRANSFER_SIZE];
    ssize_t t;

    if ( ( f = open( options.file, O_RDONLY ) ) < 0 )
    {
        genericsExit( -4, "Can't open file %s" EOL, options.file );
    }

    while ( ( t = read( f, cbw, TRANSFER_SIZE ) ) >= 0 )
    {

        if ( !t )
        {
            if ( options.endTerminate )
            {
                break;
            }
            else
            {
                // Just spin for a while to avoid clogging the CPU
                usleep( 100000 );
                continue;
            }
        }

        unsigned char *c = cbw;

        while ( t-- )
        {
            _protocolPump( *c++ );
        }
    }

    if ( !options.endTerminate )
    {
        genericsReport( V_INFO, "File read error" EOL );
    }

    close( f );
    return true;
}

// ====================================================================================================
int socketFeeder( void )

{
    int sockfd;
    struct sockaddr_in serv_addr;
    struct hostent *server;
    unsigned char cbw[TRANSFER_SIZE];
    ssize_t t;
    int flag = 1;

    #ifdef WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
    #endif

    sockfd = socket( AF_INET, SOCK_STREAM, IPPROTO_TCP );
    setsockopt( sockfd, SOL_SOCKET, SO_REUSEPORT, (const void*)&flag, sizeof( flag ) );

    if ( sockfd < 0 )
    {
        genericsReport( V_ERROR, "Error creating socket" EOL );
        return -1;
    }

    /* Now open the network connection */
    memset( ( char * ) &serv_addr, 0, sizeof( serv_addr ) );
    server = gethostbyname( options.server );

    if ( !server )
    {
        genericsReport( V_ERROR, "Cannot find host" EOL );
        return -1;
    }

    serv_addr.sin_family = AF_INET;
    memcpy( ( char * )&serv_addr.sin_addr.s_addr,
            ( const char * )server->h_addr,
            server->h_length
    );
    serv_addr.sin_port = htons( options.port );

    if ( connect( sockfd, ( struct sockaddr * ) &serv_addr, sizeof( serv_addr ) ) < 0 )
    {
        genericsReport( V_ERROR, "Could not connect" EOL );
        return -1;
    }

    // while ( ( t = read( sockfd, cbw, TRANSFER_SIZE ) ) >= 0 )
    while ( ( t = recv( sockfd, (void*)cbw, TRANSFER_SIZE, 0) ) > 0 )
    {
        unsigned char *c = cbw;

        while ( t-- )
        {
            _protocolPump( *c++ );
        }

        fflush( stdout );
    }

    genericsReport( V_ERROR, "Read failed" EOL );

    close( sockfd );
    return -2;

}

// ====================================================================================================
int main( int argc, char *argv[] )

{
    if ( !_processOptions( argc, argv ) )
    {
        exit( -1 );
    }

    /* Reset the TPIU handler before we start */
    TPIUDecoderInit( &_r.t );
    ITMDecoderInit( &_r.i, options.forceITMSync );

    if ( options.file )
    {
        exit( fileFeeder() );
    }

    do
    {
        int rc = socketFeeder();
        // TODO - make logging of failures/reconnections "nicer" based on rc?
        ( void )rc;
        // tradeoff to re-attach "promptly" vs CPU spinning and log spam
        usleep( 100 * 1000 );
    }
    while ( !options.endTerminate );
}
// ====================================================================================================
