/* SPDX-License-Identifier: BSD-3-Clause */

/*
 * Post mortem monitor for parallel trace
 * ======================================
 *
 */

#include <stdarg.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <assert.h>
#include <signal.h>

#include "git_version_info.h"
#include "generics.h"
#include "nw.h"
#include "etmDecoder.h"
#include "tpiuDecoder.h"
#include "symbols.h"
#include "sio.h"

#define REMOTE_SERVER       "localhost"

#define SCRATCH_STRING_LEN  (65535)     /* Max length for a string under construction */
//#define DUMP_BLOCK
#define DEFAULT_PM_BUFLEN_K (32)        /* Default size of the Postmortem buffer */
#define MAX_TAGS            (10)        /* How many tags we will allow */

#define INTERVAL_TIME_MS    (1000)      /* Intervaltime between acculumator resets */
#define HANG_TIME_MS        (200)       /* Time without a packet after which we dump the buffer */
#define TICK_TIME_MS        (100)       /* Time intervals for screen updates and keypress check */

/* Record for options, either defaults or from command line */
struct Options
{
    /* Source information */
    char *file;                         /* File host connection */
    bool fileTerminate;                 /* Terminate when file read isn't successful */
    char *deleteMaterial;               /* Material to delete off front end of filenames */
    bool demangle;                      /* Indicator that C++ should be demangled */

    char *elffile;                      /* File to use for symbols etc. */

    int buflen;                         /* Length of post-mortem buffer, in bytes */
    bool useTPIU;                       /* Are we using TPIU, and stripping TPIU frames? */
    int channel;                        /* When TPIU is in use, which channel to decode? */
    int port;                           /* Source information */
    char *server;
    bool noAltAddr;                     /* Flag to *not* use alternate addressing */
    char *openFileCL;                   /* Command line for opening refernced file */

} _options =
{
    .port = NWCLIENT_SERVER_PORT,
    .server = REMOTE_SERVER,
    .demangle = true,
    .channel = 2,
    .buflen = DEFAULT_PM_BUFLEN_K * 1024
};

/* A block of received data */
struct dataBlock
{
    ssize_t fillLevel;
    uint8_t buffer[TRANSFER_SIZE];
};

/* Materials required to be maintained across callbacks for output construction */
struct opConstruct
{
    uint32_t currentFileindex;           /* The filename we're currently in */
    uint32_t currentFunctionindex;       /* The function we're currently in */
    uint32_t currentLine;                /* The line we're currently in */
    uint32_t workingAddr;                /* The address we're currently in */
};


struct RunTime
{
    struct ETMDecoder i;
    struct TPIUDecoder t;

    const char *progName;               /* Name by which this program was called */
    struct SymbolSet *s;                /* Symbols read from elf */
    bool     ending;                    /* Flag indicating app is terminating */
    bool     singleShot;                /* Flag indicating take a single buffer then stop */
    uint64_t newTotalBytes;             /* Number of bytes of real data transferred in total */
    uint64_t oldTotalBytes;             /* Old number of bytes of real data transferred in total */
    uint64_t oldTotalIntervalBytes;     /* Number of bytes transferred in previous interval */
    uint64_t oldTotalHangBytes;         /* Number of bytes transferred in previous hang interval */

    uint8_t *pmBuffer;                  /* The post-mortem buffer */
    int wp;                             /* Index pointers for ring buffer */
    int rp;

    struct line *opText;                /* Text of the output buffer */
    int32_t numLines;                   /* Number of lines in the output buffer */

    int32_t diveline;                   /* Line number we're currently diving into */
    char *divefile;                     /* Filename we're currently diving into */
    bool diving;                        /* Flag indicating we're diving into a file at the moment */
    struct line *fileopText;            /* The text lines of the file we're diving into */
    int32_t filenumLines;               /* ...and how many lines of it there are */

    bool held;                          /* If we are actively collecting data */

    struct SIOInstance *sio;            /* Our screen IO instance for managed I/O */

    struct dataBlock rawBlock;          /* Datablock received from distribution */

    struct opConstruct op;              /* The mechanical elements for creating the output buffer */

    struct Options *options;            /* Our runtime configuration */
} _r =
{
    .options = &_options
};

/* For opening the editor (Shift-Right-Arrow) the following command lines work for a few editors;
 *
 * emacs; -c "emacs +%l %f"
 * codium; -c "codium  -g %f:%l"
 * eclipse; -c "eclipse %f:%l"
 */

// ====================================================================================================
// ====================================================================================================
// ====================================================================================================
// Private routines
// ====================================================================================================
// ====================================================================================================
// ====================================================================================================
static void _doExit( void ); /* Forward definition needed */
// ====================================================================================================
static void _intHandler( int sig )

/* Catch CTRL-C so things can be cleaned up properly via atexit functions */
{
    /* CTRL-C exit is not an error... */
    exit( 0 );
}
// ====================================================================================================
static void _printHelp( struct RunTime *r )

{
    genericsPrintf( "Usage: %s [options]" EOL, r->progName );
    genericsPrintf( "       -a: Do not use alternate address encoding" EOL );
    genericsPrintf( "       -b: <Length> Length of post-mortem buffer, in KBytes (Default %d KBytes)" EOL, DEFAULT_PM_BUFLEN_K );
    genericsPrintf( "       -c: <command> Command line for external editor (%f = filename, %l = line)" EOL );
    genericsPrintf( "       -D: Switch off C++ symbol demangling" EOL );
    genericsPrintf( "       -d: <String> Material to delete off front of filenames" EOL );
    genericsPrintf( "       -e: <ElfFile> to use for symbols and source" EOL );
    genericsPrintf( "       -E: When reading from file, terminate at end of file rather than waiting for further input" EOL );
    genericsPrintf( "       -f <filename>: Take input from specified file" EOL );
    genericsPrintf( "       -h: This help" EOL );
    genericsPrintf( "       -s: <Server>:<Port> to use" EOL );
    genericsPrintf( "       -t <channel>: Use TPIU to strip TPIU on specfied channel" EOL );
    genericsPrintf( "       -v: <level> Verbose mode 0(errors)..3(debug)" EOL );
    genericsPrintf( EOL "(Will connect one port higher than that set in -s when TPIU is not used)" EOL );
    genericsPrintf( EOL "(this will automatically select the second output stream from orb TPIU.)" EOL );
    genericsPrintf( EOL "Environment Variables;" EOL );
    genericsPrintf( "  OBJDUMP: to use non-standard obbdump binary" EOL );
}
// ====================================================================================================
static int _processOptions( int argc, char *argv[], struct RunTime *r )

{
    int c;

    while ( ( c = getopt ( argc, argv, "ab:c:Dd:Ee:f:hs:t:v:" ) ) != -1 )
        switch ( c )
        {
            // ------------------------------------
            case 'a':
                r->options->noAltAddr = true;
                break;

            // ------------------------------------
            case 'b':
                r->options->buflen = atoi( optarg ) * 1024;
                break;

            // ------------------------------------
            case 'c':
                r->options->openFileCL = optarg;
                break;

            // ------------------------------------
            case 'D':
                r->options->demangle = false;
                break;

            // ------------------------------------
            case 'd':
                r->options->deleteMaterial = optarg;
                break;

            // ------------------------------------
            case 'E':
                r->options->fileTerminate = true;
                break;

            // ------------------------------------

            case 'e':
                r->options->elffile = optarg;
                break;

            // ------------------------------------

            case 'f':
                r->options->file = optarg;
                break;

            // ------------------------------------

            case 'h':
                _printHelp( r );
                return false;

            // ------------------------------------

            case 's':
                r->options->server = optarg;

                // See if we have an optional port number too
                char *a = optarg;

                while ( ( *a ) && ( *a != ':' ) )
                {
                    a++;
                }

                if ( *a == ':' )
                {
                    *a = 0;
                    r->options->port = atoi( ++a );
                }

                if ( !r->options->port )
                {
                    r->options->port = NWCLIENT_SERVER_PORT;
                }

                break;

            // ------------------------------------

            case 't':
                r->options->useTPIU = true;
                r->options->channel = atoi( optarg );
                break;

            // ------------------------------------

            case 'v':
                genericsSetReportLevel( atoi( optarg ) );
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
                genericsReport( V_ERROR, "Unrecognised option '%c'" EOL, c );
                return false;
                // ------------------------------------
        }

    /* ... and dump the config if we're being verbose */
    genericsReport( V_INFO, "%s V" VERSION " (Git %08X %s, Built " BUILD_DATE ")" EOL, argv[0], GIT_HASH, ( GIT_DIRTY ? "Dirty" : "Clean" ) );

    if ( !r->options->elffile )
    {
        genericsExit( V_ERROR, "Elf File not specified" EOL );
    }

    if ( !r->options->buflen )
    {
        genericsExit( -1, "Illegal value for Post Mortem Buffer length" EOL );
    }

    return true;
}
// ====================================================================================================
static void _processBlock( struct RunTime *r )

/* Generic block processor for received data */

{
    uint8_t *c = r->rawBlock.buffer;
    uint32_t y = r->rawBlock.fillLevel;

    genericsReport( V_DEBUG, "RXED Packet of %d bytes" EOL, y );

    if ( y )
    {
#ifdef DUMP_BLOCK
        fprintf( stderr, EOL );

        while ( y-- )
        {
            fprintf( stderr, "%02X ", *c++ );

            if ( !( y % 16 ) )
            {
                fprintf( stderr, EOL );
            }
        }

        c = r->rawBlock.buffer;
        y = r->rawBlock.fillLevel;
#endif

        if ( r->options->useTPIU )
        {
            struct TPIUPacket p;

            while ( y-- )
            {
                if ( TPIU_EV_RXEDPACKET == TPIUPump( &r->t, *c++ ) )
                {
                    if ( !TPIUGetPacket( &r->t, &p ) )
                    {
                        genericsReport( V_WARN, "TPIUGetPacket fell over" EOL );
                    }
                    else
                    {
                        /* Iterate through the packet, putting bytes for ETM into the processing buffer */
                        for ( uint32_t g = 0; g < p.len; g++ )
                        {
                            if ( r->options->channel == p.packet[g].s )
                            {
                                r->pmBuffer[r->wp] = p.packet[g].d;
                                r->newTotalBytes++;
                                uint32_t nwp = ( r->wp + 1 ) % r->options->buflen;

                                if ( nwp == r->rp )
                                {
                                    if ( r->singleShot )
                                    {
                                        r->held = true;
                                        return;
                                    }
                                    else
                                    {
                                        r->rp = ( r->rp + 1 ) % r->options->buflen;
                                    }
                                }

                                r->wp = nwp;
                            }
                        }
                    }
                }
            }
        }
        else
        {
            r->newTotalBytes += y;

            while ( y-- )
            {
                r->pmBuffer[r->wp] = *c++;
                uint32_t nwp = ( r->wp + 1 ) % r->options->buflen;

                if ( nwp == r->rp )
                {
                    if ( r->singleShot )
                    {
                        r->held = true;
                        return;
                    }
                    else
                    {
                        r->rp = ( r->rp + 1 ) % r->options->buflen;
                    }
                }

                r->wp = nwp;
            }
        }
    }
}
// ====================================================================================================
static void _flushBuffer( struct RunTime *r )

/* Empty the output buffer, and de-allocate its memory */

{
    /* Tell the UI there's nothing more to show */
    SIOsetOutputBuffer( r->sio, 0, 0, NULL, false );

    /* Remove all of the recorded lines */
    while ( r->numLines-- )
    {
        if ( !r->opText[r->numLines].isRef )
        {
            free( r->opText[r->numLines].buffer );
        }
    }

    /* and the opText buffer */
    free( r->opText );
    r->opText = NULL;
    r->numLines = 0;

    /* ...and the file/line references */
    r->op.currentLine = NO_LINE;
    r->op.currentFileindex = NO_FILE;
    r->op.currentFunctionindex = NO_FUNCTION;
    r->op.workingAddr = NO_DESTADDRESS;
}
// ====================================================================================================
static void _appendToOPBuffer( struct RunTime *r, int32_t lineno, enum LineType lt, const char *fmt, ... )

/* Add line to output buffer, in a printf stylee */

{
    char construct[SCRATCH_STRING_LEN];
    va_list va;
    char *p;

    va_start( va, fmt );
    vsnprintf( construct, SCRATCH_STRING_LEN, fmt, va );
    va_end( va );

    /* Make sure we didn't accidentially admit a CR or LF */
    for ( p = construct; ( ( *p ) && ( *p != '\n' ) && ( *p != '\r' ) ); p++ );

    *p = 0;

    r->opText = ( struct line * )realloc( r->opText, ( sizeof( struct line ) ) * ( r->numLines + 1 ) );
    r->opText[r->numLines].buffer = strdup( construct );
    r->opText[r->numLines].lt     = lt;
    r->opText[r->numLines].line   = lineno;
    r->opText[r->numLines].isRef  = false;
    r->numLines++;
}
// ====================================================================================================
static void _appendRefToOPBuffer( struct RunTime *r, int32_t lineno, enum LineType lt, const char *ref )

/* Add line to output buffer, as a reference (which don't be free'd later) */

{
    r->opText = ( struct line * )realloc( r->opText, ( sizeof( struct line ) ) * ( r->numLines + 1 ) );

    /* This line removes the 'const', but we know to not mess with this line */
    r->opText[r->numLines].buffer = ( char * )ref;
    r->opText[r->numLines].lt     = lt;
    r->opText[r->numLines].line   = lineno;
    r->opText[r->numLines].isRef  = true;
    r->numLines++;
}
// ====================================================================================================
static void _etmReport( enum verbLevel l, const char *fmt, ... )

/* Debug reporting stream */

{
    static char op[SCRATCH_STRING_LEN];

    va_list va;
    va_start( va, fmt );
    vsnprintf( op, SCRATCH_STRING_LEN, fmt, va );
    va_end( va );
    _appendToOPBuffer( &_r, _r.op.currentLine, LT_DEBUG, op );
}
// ====================================================================================================
static void _etmCB( void *d )

/* Callback function for when valid ETM decode is detected */

{
    struct RunTime *r = ( struct RunTime * )d;
    struct ETMCPUState *cpu = ETMCPUState( &r->i );
    char construct[SCRATCH_STRING_LEN];
    uint32_t incAddr = 0;
    struct nameEntry n;
    uint32_t disposition;

    /* Deal with changes introduced by this event ========================= */
    if ( ETMStateChanged( &r->i, EV_CH_ADDRESS ) )
    {
        /* Make debug report if calculated and reported addresses differ. This is most useful for testing when exhaustive  */
        /* address reporting is switched on. It will give 'false positives' for uncalculable instructions (e.g. bx lr) but */
        /* it's a decent safety net to be sure the jump decoder is working correctly.                                      */
        _appendToOPBuffer( r, r->op.currentLine, LT_DEBUG, "%sCommanded CPU Address change (Was:0x%08x Commanded:0x%08x)" EOL,
                           ( r->op.workingAddr == cpu->addr ) ? "" : "***INCONSISTENT*** ", r->op.workingAddr, cpu->addr );

        r->op.workingAddr = cpu->addr;
    }

    if ( ETMStateChanged( &r->i, EV_CH_ENATOMS ) )
    {
        incAddr = cpu->eatoms + cpu->natoms;
        disposition = cpu->disposition;
    }

    if ( ETMStateChanged( &r->i, EV_CH_VMID ) )
    {
        _appendToOPBuffer( r, r->op.currentLine, LT_EVENT, "*** VMID Set to %d", cpu->vmid );
    }

    if ( ETMStateChanged( &r->i, EV_CH_EX_ENTRY ) )
    {
        _appendToOPBuffer( r, r->op.currentLine, LT_EVENT, "========== Exception Entry%s (%d at 0x%08x) ==========",
                           ETMStateChanged( &r->i, EV_CH_CANCELLED ) ? ", Last Instruction Cancelled" : "", cpu->exception, cpu->addr );
    }

    if ( ETMStateChanged( &r->i, EV_CH_EX_EXIT ) )
    {
        _appendRefToOPBuffer( r, r->op.currentLine, LT_EVENT, "========== Exception Exit ==========" );
    }

    if ( ETMStateChanged( &r->i, EV_CH_TSTAMP ) )
    {
        _appendToOPBuffer( r, r->op.currentLine, LT_EVENT, "*** Timestamp %ld", cpu->ts );
    }

    if ( ETMStateChanged( &r->i, EV_CH_TRIGGER ) )
    {
        _appendRefToOPBuffer( r, r->op.currentLine, LT_EVENT, "*** Trigger" );
    }

    if ( ETMStateChanged( &r->i, EV_CH_CLOCKSPEED ) )
    {
        _appendRefToOPBuffer( r, r->op.currentLine, LT_EVENT, "*** Change Clockspeed" );
    }

    if ( ETMStateChanged( &r->i, EV_CH_ISLSIP ) )
    {
        _appendRefToOPBuffer( r, r->op.currentLine, LT_EVENT, "*** ISLSIP Triggered" );
    }

    if ( ETMStateChanged( &r->i, EV_CH_CYCLECOUNT ) )
    {
        _appendToOPBuffer( r, r->op.currentLine, LT_EVENT, "(Cycle Count %d)", cpu->cycleCount );
    }

    if ( ETMStateChanged( &r->i, EV_CH_VMID ) )
    {
        _appendToOPBuffer( r, r->op.currentLine, LT_EVENT, "(VMID is now %d)", cpu->vmid );
    }

    if ( ETMStateChanged( &r->i, EV_CH_CONTEXTID ) )
    {
        _appendToOPBuffer( r, r->op.currentLine, LT_EVENT, "(Context ID is now %d)", cpu->contextID );
    }

    if ( ETMStateChanged( &r->i, EV_CH_SECURE ) )
    {
        _appendToOPBuffer( r, r->op.currentLine, LT_EVENT, "(Non-Secure State is now %s)", cpu->nonSecure ? "True" : "False" );
    }

    if ( ETMStateChanged( &r->i, EV_CH_ALTISA ) )
    {
        _appendToOPBuffer( r, r->op.currentLine, LT_EVENT, "(Using AltISA  is now %s)", cpu->altISA ? "True" : "False" );
    }

    if ( ETMStateChanged( &r->i, EV_CH_HYP ) )
    {
        _appendToOPBuffer( r, r->op.currentLine,  LT_EVENT, "(Using Hypervisor is now %s)", cpu->hyp ? "True" : "False" );
    }

    if ( ETMStateChanged( &r->i, EV_CH_JAZELLE ) )
    {
        _appendToOPBuffer( r, r->op.currentLine, LT_EVENT, "(Using Jazelle is now %s)", cpu->jazelle ? "True" : "False" );
    }

    if ( ETMStateChanged( &r->i, EV_CH_THUMB ) )
    {
        _appendToOPBuffer( r, r->op.currentLine, LT_EVENT, "(Using Thumb is now %s)", cpu->thumb ? "True" : "False" );
    }

    /* End of dealing with changes introduced by this event =============== */

    while ( incAddr )
    {
        incAddr--;

        if ( SymbolLookup( r->s, r->op.workingAddr, &n ) )
        {
            /* If we have changed file or function put a header line in */
            if ( ( n.fileindex != r->op.currentFileindex ) || ( n.functionindex != r->op.currentFunctionindex ) )
            {
                _appendToOPBuffer( r, r->op.currentLine, LT_FILE, "%s::%s", SymbolFilename( r->s, n.fileindex ), SymbolFunction( r->s, n.functionindex ) );
                r->op.currentFileindex     = n.fileindex;
                r->op.currentFunctionindex = n.functionindex;
                r->op.currentLine = NO_LINE;
            }

            /* If we have changed line then output the new one */
            if ( n.line != r->op.currentLine - 1 )
            {
                const char *v = n.source;
                r->op.currentLine = n.line - n.linesInBlock + 1;
                *construct = 0;

                if ( v ) while ( *v )
                    {
                        /* In buffer output NL/CR are treated as end of string, so this is safe */
                        /* with these buffers that can span multiple lines. Split into separate ones. */
                        _appendRefToOPBuffer( r, r->op.currentLine++, LT_SOURCE, v );

                        /* Move to the CR/NL or EOL on this line */
                        while ( ( *v ) && ( *v != '\r' ) && ( *v != '\n' ) )
                        {
                            v++;
                        }

                        if ( *v )
                        {
                            /* Found end of string or NL/CR...move past those */
                            if ( ( ( *v == '\r' ) && ( *( v + 1 ) == '\n' ) ) ||
                                    ( ( *v == '\n' ) && ( *( v + 1 ) == '\r' ) )
                               )
                            {
                                v += 2;
                            }
                            else
                            {
                                v++;
                            }
                        }

                    }
            }

            /* If this line has assembly then output it */
            if ( n.assyLine != ASSY_NOT_FOUND )
            {
                _appendRefToOPBuffer( r, r->op.currentLine, ( disposition & 1 ) ? LT_ASSEMBLY : LT_NASSEMBLY, n.assy[n.assyLine].lineText );

                if ( n.assy[n.assyLine].isJump || n.assy[n.assyLine].isSubCall )
                {
                    _appendToOPBuffer( r, r->op.currentLine, LT_DEBUG, "%sTAKEN %s", ( disposition & 1 ) ? "" : "NOT ", n.assy[n.assyLine].isJump ? "JUMP" : "SUBCALL"  );
                }

                if ( ( n.assy[n.assyLine].isJump || n.assy[n.assyLine].isSubCall ) && ( disposition & 1 ) )
                {
                    /* This is a fixed jump that _was_ taken, so update working address */
                    r->op.workingAddr = n.assy[n.assyLine].jumpdest;
                }
                else
                {
                    r->op.workingAddr += ( n.assy[n.assyLine].is4Byte ) ? 4 : 2;
                }
            }
            else
            {
                _appendRefToOPBuffer( r, r->op.currentLine, LT_ASSEMBLY, "\t\tASSEMBLY NOT FOUND" EOL );
                r->op.workingAddr += 2;
            }
        }
        else
        {
            /* We didn't have a symbol for this address, so let's just assume a short instruction */
            _appendRefToOPBuffer( r, r->op.currentLine, LT_DEBUG, "*** No Symbol found ***" EOL );
            r->op.workingAddr += 2;
        }

        disposition >>= 1;
    }
}
// ====================================================================================================
static void _dumpBuffer( struct RunTime *r )

/* Dump received data buffer into text buffer */

{
    _flushBuffer( r );

    if ( !SymbolSetValid( &r->s, r->options->elffile ) )
    {
        if ( !( r->s = SymbolSetCreate( r->options->elffile, r->options->deleteMaterial, r->options->demangle, true, true ) ) )
        {
            genericsReport( V_ERROR, "Elf file or symbols in it not found" EOL );
            return;
        }
        else
        {
            genericsReport( V_DEBUG, "Loaded %s" EOL, r->options->elffile );
        }
    }

    /* Pump the received messages through the ETM decoder, it will callback to _etmCB with complete sentences */
    int bytesAvailable = ( ( r->wp + r->options->buflen ) - r->rp ) % r->options->buflen;

    /* If we started wrapping (i.e. the rx ring buffer got full) then any guesses about sync status are invalid */
    if ( ( bytesAvailable == r->options->buflen - 1 ) && ( !r->singleShot ) )
    {
        ETMDecoderForceSync( &r->i, false );
    }

    if ( ( bytesAvailable + r->rp ) > r->options->buflen )
    {
        /* Buffer is wrapped - submit both parts */
        ETMDecoderPump( &r->i, &r->pmBuffer[r->rp], r->options->buflen - r->rp, _etmCB, _etmReport, r );
        ETMDecoderPump( &r->i, &r->pmBuffer[0], r->wp, _etmCB, _etmReport, r );
    }
    else
    {
        /* Buffer is not wrapped */
        ETMDecoderPump( &r->i, &r->pmBuffer[r->rp], bytesAvailable, _etmCB, _etmReport, r );
    }

    /* Submit this constructed buffer for display */
    SIOsetOutputBuffer( r->sio, r->numLines, r->numLines - 1, &r->opText, false );
}
// ====================================================================================================
static bool _currentFileAndLine( struct RunTime *r, char **file, int32_t *l )

{
    /* Search backwards from current file and line until we find (a) a source line with a line number and */
    /* (b) a filename which contains this line. */

    int32_t sl = 0;
    int32_t i = SIOgetCurrentLineno( r->sio );
    *file = NULL;

    while ( ( i ) && ( r->opText[i].lt != LT_FILE ) )
    {
        if ( ( r->opText[i].lt == LT_SOURCE ) && ( !sl ) )
        {
            sl = r->opText[i].line;
        }

        i--;
    }

    if ( r->opText[i].lt == LT_FILE )
    {
        *file = r->opText[i].buffer;
    }

    if ( !sl )
    {
        /* This was odd, no line number found before filename was. Let's search forward for one */
        while ( ( i < r->numLines ) && ( r->opText[i].lt != LT_SOURCE ) )
        {
            i++;
        }

        if ( i < r->numLines )
        {
            sl = r->opText[i].line;
        }
    }

    if ( ( !sl ) || ( !*file ) )
    {
        /* We didn't find everything */
        return false;
    }

    *l = sl;
    return true;
}
// ====================================================================================================
static void _openFileCommand( struct RunTime *r, int32_t line, char *fileToOpen )

{
    char construct[SCRATCH_STRING_LEN];
    char *a = r->options->openFileCL;
    char *b = construct;

    /* We now have the filename and line number that we're targetting...go collect this file */

    while ( *a )
    {
        if ( *a != '%' )
        {
            *b++ = *a++;
        }
        else
        {
            a++;

            if ( *a == 'f' )
            {
                a++;
                b += snprintf( b, SCRATCH_STRING_LEN - ( b - construct ), "%s", fileToOpen );
            }
            else if ( *a == 'l' )
            {
                a++;
                b += snprintf( b, SCRATCH_STRING_LEN - ( b - construct ), "%d", line );
            }
            else
            {
                *b++ = *a++;
            }
        }
    }

    *b++ = ' ';
    *b++ = '&';
    *b++ = 0;

    /* Now detach from controlling terminal and send the message */
    system( construct );
}
// ====================================================================================================
static void _openFileBuffer( struct RunTime *r, int32_t line, char *fileToOpen )

/* Read file into buffer */

{
    FILE *f;
    char construct[SCRATCH_STRING_LEN];
    char *p;
    int32_t lc = 0;

    f = fopen( fileToOpen, "r" );

    if ( !f )
    {
        SIOalert( r->sio, "Couldn't open file" );
        return;
    }

    while ( !feof( f ) )
    {
        if ( !fgets( construct, SCRATCH_STRING_LEN, f ) )
        {
            break;
        }

        lc++;
        r->fileopText = ( struct line * )realloc( r->fileopText, ( sizeof( struct line ) ) * ( r->filenumLines + 1 ) );

        /* Remove and LF/CR */
        for ( p = construct; ( ( *p ) && ( *p != '\n' ) && ( *p != '\r' ) ); p++ );

        *p = 0;

        r->fileopText[r->filenumLines].buffer = strdup( construct );
        r->fileopText[r->filenumLines].lt     = LT_MU_SOURCE;
        r->fileopText[r->filenumLines].line   = lc;
        r->fileopText[r->filenumLines].isRef  = false;
        r->filenumLines++;
    }

    fclose( f );

    SIOsetOutputBuffer( r->sio, r->filenumLines, line - 1, &r->fileopText, true );
    r->diving = true;
}
// ====================================================================================================
static void _doFileOpen( struct RunTime *r, bool isDive )

/* Do actions required to open file to dive into */

{
    char *p;
    char *filename;
    int32_t lineNo;
    char construct[SCRATCH_STRING_LEN];

    if ( ( r->diving ) || ( !r->numLines ) || ( !r->held ) )
    {
        return;
    }

    /* There should be no file read in at the moment */
    assert( !r->fileopText );
    assert( !r->filenumLines );


    if ( !_currentFileAndLine( r, &p, &lineNo ) )
    {
        SIOalert( r->sio, "Couldn't get filename/line" );
        return;
    }

    filename = strdup( p );
    p = &filename[strlen( filename )];

    /* Roll back to before the '::' to turn this into a filename */
    while ( ( p != filename ) && ( *p != ':' ) )
    {
        p--;
    }

    if ( ( p == filename ) || ( *( p - 1 ) != ':' ) )
    {
        SIOalert( r->sio, "Couldn't decode filename" );
        free( filename );
        return;
    }

    *( p - 1 ) = 0;

    /* Now create filename including stripped material if need be */
    snprintf( construct, SCRATCH_STRING_LEN, "%s%s", r->options->deleteMaterial ? r->options->deleteMaterial : "", filename );

    if ( isDive )
    {
        _openFileBuffer( r, lineNo, construct );
    }
    else
    {
        _openFileCommand( r, lineNo, construct );
    }

    free( filename );
}
// ====================================================================================================
static void _doFilesurface( struct RunTime *r )

/* Come back out of a file we're diving into */

{
    if ( !r->diving )
    {
        return;
    }

    while ( r->filenumLines )
    {
        free( r->fileopText[--r->filenumLines].buffer );
    }

    r->fileopText = NULL;
    r->diving = false;
    SIOsetOutputBuffer( r->sio, r->numLines, r->numLines - 1, &r->opText, false );
}
// ====================================================================================================
static void _doSave( struct RunTime *r )

/* Save buffer in both raw and processed formats */

{
    FILE *f;
    char fn[SCRATCH_STRING_LEN];
    uint32_t w;
    char *p;

    snprintf( fn, SCRATCH_STRING_LEN, "%s.trace", SIOgetSaveFilename( r->sio ) );
    f = fopen( fn, "wb" );

    if ( !f )
    {
        SIOalert( r->sio, "Save Trace Failed" );
        return;
    }

    w = r->rp;

    while ( w != r->wp )
    {
        fwrite( &r->pmBuffer[w], 1, 1, f );
        w = ( w + 1 ) % r->options->buflen;
    }

    fclose( f );

    snprintf( fn, SCRATCH_STRING_LEN, "%s.report", SIOgetSaveFilename( r->sio ) );
    f = fopen( fn, "wb" );

    if ( !f )
    {
        SIOalert( r->sio, "Save Report Failed" );
        return;
    }

    w = 0;

    while ( w != r->numLines )
    {
        p = r->opText[w].buffer;

        if ( ( r->opText[w].lt == LT_SOURCE ) || ( r->opText[w].lt == LT_MU_SOURCE ) )
        {
            /* Need a line number on this */
            fwrite( fn, sprintf( fn, "%5d ", r->opText[w].line ), 1, f );
        }

        if ( r->opText[w].lt == LT_NASSEMBLY )
        {
            /* This is an _unexecuted_ assembly line, need to mark it */
            fwrite( "(**", 3, 1, f );
        }

        /* Search forward for a NL or 0, both are EOL for this purpose */
        while ( ( *p ) && ( *p != '\n' ) && ( *p != '\r' ) )
        {
            p++;
        }

        fwrite( r->opText[w].buffer, p - r->opText[w].buffer, 1, f );

        if ( r->opText[w].lt == LT_NASSEMBLY )
        {
            /* This is an _unexecuted_ assembly line, need to mark it */
            fwrite( " **)", 4, 1, f );
        }

        fwrite( EOL, strlen( EOL ), 1, f );
        w++;
    }

    fclose( f );

    SIOalert( r->sio, "Save Complete" );
}
// ====================================================================================================
static void _doExit( void )

/* Perform any explicit exit functions */

{
    _r.ending = true;
    /* Give them a bit of time, then we're leaving anyway */
    usleep( 200 );
    SIOterminate( _r.sio );
}
// ====================================================================================================
int main( int argc, char *argv[] )

{
    int sourcefd;
    struct sockaddr_in serv_addr;
    struct hostent *server;
    int flag = 1;

    int32_t lastTTime, lastTSTime, lastHTime;
    int r;
    struct timeval tv;
    fd_set readfds;

    /* Have a basic name and search string set up */
    _r.progName = genericsBasename( argv[0] );

    if ( !_processOptions( argc, argv, &_r ) )
    {
        /* processOptions generates its own error messages */
        genericsExit( -1, "" EOL );
    }

    /* Make sure the fifos get removed at the end */
    atexit( _doExit );

    /* Create a screen and interaction handler */
    _r.sio = SIOsetup( _r.progName, _r.options->elffile, ( _r.options->file != NULL ) );

    /* Fill in a time to start from */
    lastHTime = lastTTime = lastTSTime = genericsTimestampmS();

    /* This ensures the atexit gets called */
    if ( SIG_ERR == signal( SIGINT, _intHandler ) )
    {
        genericsExit( -1, "Failed to establish Int handler" EOL );
    }

    /* Don't kill a sub-process when any reader or writer evaporates */
    if ( SIG_ERR == signal( SIGPIPE, SIG_IGN ) )
    {
        genericsExit( -1, "Failed to ignore SIGPIPEs" EOL );
    }

    /* Create the buffer memory */
    _r.pmBuffer = ( uint8_t * )calloc( 1, _r.options->buflen );

    ETMDecoderInit( &_r.i, !( _r.options->noAltAddr ) );

    if ( _r.options->useTPIU )
    {
        TPIUDecoderInit( &_r.t );
    }

    while ( !_r.ending )
    {
        if ( !_r.options->file )
        {
            /* Get the socket open */
            sourcefd = socket( AF_INET, SOCK_STREAM, 0 );
            setsockopt( sourcefd, SOL_SOCKET, SO_REUSEPORT, &flag, sizeof( flag ) );

            if ( sourcefd < 0 )
            {
                perror( "Error creating socket\n" );
                return -EIO;
            }

            if ( setsockopt( sourcefd, SOL_SOCKET, SO_REUSEADDR, &( int )
        {
            1
        }, sizeof( int ) ) < 0 )
            {
                perror( "setsockopt(SO_REUSEADDR) failed" );
                return -EIO;
            }

            /* Now open the network connection */
            bzero( ( char * ) &serv_addr, sizeof( serv_addr ) );
            server = gethostbyname( _r.options->server );

            if ( !server )
            {
                perror( "Cannot find host" EOL );
                return -EIO;
            }

            serv_addr.sin_family = AF_INET;
            bcopy( ( char * )server->h_addr,
                   ( char * )&serv_addr.sin_addr.s_addr,
                   server->h_length );
            serv_addr.sin_port = htons( _r.options->port + ( _r.options->useTPIU ? 0 : 1 ) );

            if ( connect( sourcefd, ( struct sockaddr * ) &serv_addr, sizeof( serv_addr ) ) < 0 )
            {
                /* This can happen when the feeder has gone missing... */
                close( sourcefd );
                usleep( 1000000 );
                continue;
            }
        }
        else
        {

            if ( ( sourcefd = open( _r.options->file, O_RDONLY ) ) < 0 )
            {
                genericsExit( sourcefd, "Can't open file %s" EOL, _r.options->file );
            }
        }

        FD_ZERO( &readfds );

        /* ----------------------------------------------------------------------------- */
        /* This is the main active loop...only break out of this when ending or on error */
        /* ----------------------------------------------------------------------------- */
        while ( !_r.ending )
        {
            /* Each time segment is restricted to 10mS */
            tv.tv_sec = 0;
            tv.tv_usec  = 10000;

            if ( sourcefd )
            {
                FD_SET( sourcefd, &readfds );
            }

            FD_SET( STDIN_FILENO, &readfds );

            r = select( sourcefd ? sourcefd + 1 : STDIN_FILENO + 1, &readfds, NULL, NULL, &tv );

            if ( r < 0 )
            {
                /* Something went wrong in the select */
                break;
            }

            if ( sourcefd && FD_ISSET( sourcefd, &readfds ) )
            {
                /* We always read the data, even if we're held, to keep the socket alive */
                _r.rawBlock.fillLevel = read( sourcefd, _r.rawBlock.buffer, TRANSFER_SIZE );

                if ( ( _r.rawBlock.fillLevel <= 0 ) && _r.options->file )
                {
                    /* Read from file is complete, remove fd */
                    close( sourcefd );
                    sourcefd = 0;
                }

                if ( !_r.held )
                {
                    /* Pump all of the data through the protocol handler */
                    _processBlock( &_r );
                }
            }

            /* Update the outputs and deal with any keys that made it up this high */
            switch ( SIOHandler( _r.sio, ( genericsTimestampmS() - lastTTime ) > TICK_TIME_MS, _r.oldTotalIntervalBytes ) )
            {
                case SIO_EV_HOLD:
                    if ( !_r.options->file )
                    {
                        _r.held = !_r.held;

                        if ( !_r.held )
                        {
                            _r.wp = _r.rp = 0;

                            if ( _r.diving )
                            {
                                _doFilesurface( &_r );
                            }

                            _flushBuffer( &_r );
                        }

                        /* Flag held status to the UI */
                        SIOheld( _r.sio, _r.held );
                    }

                    break;

                case SIO_EV_SAVE:
                    if ( _r.options->file )
                    {
                        _doSave( &_r );
                    }

                    break;

                case SIO_EV_DIVE:
                    _doFileOpen( &_r, true );
                    break;

                case SIO_EV_FOPEN:
                    if ( _r.options->openFileCL )
                    {
                        _doFileOpen( &_r, false );
                    }

                    break;

                case SIO_EV_SURFACE:
                    _doFilesurface( &_r );
                    break;

                case SIO_EV_QUIT:
                    _r.ending = true;
                    break;

                default:
                    break;
            }

            /* Deal with possible timeout on sampling, or if this is a read-from-file that is finished */
            if ( ( !_r.numLines )  &&
                    (
                                ( _r.options->file && !sourcefd ) ||

                                ( ( ( genericsTimestampmS() - lastHTime ) > HANG_TIME_MS ) &&
                                  ( _r.newTotalBytes - _r.oldTotalHangBytes == 0 ) &&
                                  ( _r.wp != _r.rp ) )
                    )
               )
            {
                _dumpBuffer( &_r );
                _r.held = true;
                SIOheld( _r.sio, _r.held );
            }

            /* Update the intervals */
            if ( ( genericsTimestampmS() - lastHTime ) > HANG_TIME_MS )
            {
                _r.oldTotalHangBytes = _r.newTotalBytes;
                lastHTime = genericsTimestampmS();
            }

            if ( ( genericsTimestampmS() - lastTTime ) > TICK_TIME_MS )
            {
                lastTTime = genericsTimestampmS();
            }

            if ( ( genericsTimestampmS() - lastTSTime ) > INTERVAL_TIME_MS )
            {
                _r.oldTotalIntervalBytes = _r.newTotalBytes - _r.oldTotalBytes;
                _r.oldTotalBytes = _r.newTotalBytes;
                lastTSTime = genericsTimestampmS();
            }
        }

        /* ----------------------------------------------------------------------------- */
        /* End of main loop ... we get here because something forced us out              */
        /* ----------------------------------------------------------------------------- */

        if ( sourcefd )
        {
            close( sourcefd );
        }

        if ( _r.options->file )
        {
            /* Don't keep re-reading the file if it is a file! */
            _r.held = true;
        }

        if ( _r.options->fileTerminate )
        {
            _r.ending = true;
        }
    }

    return OK;
}
// ====================================================================================================
