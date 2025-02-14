/* SPDX-License-Identifier: BSD-3-Clause */

/*
 * TPIU Decoder Module
 * ===================
 *
 */

#include <stdio.h>
#include <string.h>
#ifdef DEBUG
    #include "generics.h"
#else
    #define genericsReport(x...)
#endif
#include "tpiuDecoder.h"

#define SYNCPATTERN 0xFFFFFF7F
#define HALFSYNC_HIGH 0x7F
#define HALFSYNC_LOW  0XFF
#define NO_CHANNEL_CHANGE (0xFF)
#define TIMEOUT (3)
#define STAT_SYNC_BYTE (0xA6)

// ====================================================================================================
void TPIUDecoderInit( struct TPIUDecoder *t )

/* Reset a TPIUDecoder instance */

{
    t->state = TPIU_UNSYNCED;
    t->syncMonitor = 0;
    TPIUDecoderZeroStats( t );
}
// ====================================================================================================
void TPIUDecoderZeroStats( struct TPIUDecoder *t )

{
    memset( &t->stats, 0, sizeof( struct TPIUDecoderStats ) );
}
// ====================================================================================================
bool TPIUDecoderSynced( struct TPIUDecoder *t )
{
    return t->state != TPIU_UNSYNCED;
}
// ====================================================================================================
struct TPIUDecoderStats *TPIUDecoderGetStats( struct TPIUDecoder *t )
{
    return &t->stats;
}
// ====================================================================================================
void TPIUDecoderForceSync( struct TPIUDecoder *t, uint8_t offset )

/* Force the decoder into a specific sync state */

{
    if ( t->state == TPIU_UNSYNCED )
    {
        t->stats.syncCount++;
    }

    t->state = TPIU_RXING;
    t->byteCount = offset;

    /* Consider this a valid timestamp */
    gettimeofday( &t->lastPacket, NULL );
}
// ====================================================================================================
bool TPIUGetPacket( struct TPIUDecoder *t, struct TPIUPacket *p )

/* Copy received packet into transfer buffer, and reset receiver */

{
    uint8_t delayedStreamChange = NO_CHANNEL_CHANGE;

    /* This should have been reset in the call */
    if ( ( t->byteCount ) || ( !p ) )
    {
        return false;
    }

    p->len = 0;
    uint8_t lowbits = t->rxedPacket[TPIU_PACKET_LEN - 1];

    for ( uint32_t i = 0; i < TPIU_PACKET_LEN; i += 2 )
    {
        if ( ( t->rxedPacket[i] ) & 1 )
        {
            /* This is a stream change - either before or after the data byte */
            if ( lowbits & 1 )
            {
                delayedStreamChange = t->rxedPacket[i] >> 1;
            }
            else
            {
                t->currentStream = t->rxedPacket[i] >> 1;
            }
        }
        else
        {
            /* This is a data byte - store it */
            p->packet[p->len].d = t->rxedPacket[i] | ( lowbits & 1 );
            p->packet[p->len].s = t->currentStream;
            p->len++;
        }

        /* Now deal with the second byte of the pair */
        if ( i < 14 )
        {
            /* Now deal with the other byte of the pair ... this is always data */
            p->packet[p->len].d = t->rxedPacket[i + 1];
            p->packet[p->len].s = t->currentStream;
            p->len++;
        }

        /* ... and finally, if there's a delayed channel change, deal with it */
        if ( delayedStreamChange != NO_CHANNEL_CHANGE )
        {
            t->currentStream = delayedStreamChange;
            delayedStreamChange = NO_CHANNEL_CHANGE;
        }

        /* Make sure we accomodate the lowbit for the next two bytes */
        lowbits >>= 1;
    }

    return true;
}
// ====================================================================================================
struct TPIUCommsStats *TPIUGetCommsStats( struct TPIUDecoder *t )

{
    return &t->commsStats;
}

// ====================================================================================================
void _decodeCommsStats( struct TPIUDecoder *t )

/* Decode received communication stats into transfer buffer */

{
    t->commsStats.pendingCount = ( t->rxedPacket[2] << 8 ) | t->rxedPacket[1];
    t->commsStats.leds         = t->rxedPacket[5];
    t->commsStats.lostFrames   = ( t->rxedPacket[7] << 8 ) | t->rxedPacket[6];
    t->commsStats.totalFrames  = ( t->rxedPacket[11] << 24 ) | ( t->rxedPacket[10] << 16 ) | ( t->rxedPacket[9] << 8 ) | ( t->rxedPacket[8] );
}
// ====================================================================================================
enum TPIUPumpEvent TPIUPump( struct TPIUDecoder *t, uint8_t d )

/* Pump next byte into the protocol decoder */

{
    struct timeval nowTime, diffTime;

    t->syncMonitor = ( t->syncMonitor << 8 ) | d;

    if ( t->syncMonitor == SYNCPATTERN )
    {

        enum TPIUPumpEvent r;

        if ( t->state != TPIU_UNSYNCED )
        {
            r = TPIU_EV_SYNCED;
        }
        else
        {
            r = TPIU_EV_NEWSYNC;
        }

        /* Deal with the special state that these are communication stats from the link */
        /* ...it is still a reset though!                                               */
        if ( ( t->byteCount == 14 ) && ( t->rxedPacket[0] == STAT_SYNC_BYTE ) )
        {
            _decodeCommsStats( t );
        }

        t->state = TPIU_RXING;
        t->stats.syncCount++;
        t->byteCount = 0;
        t->got_lowbits = false;
        genericsReport( V_DEBUG, "!!!! " EOL );

        /* Consider this a valid timestamp */
        gettimeofday( &t->lastPacket, NULL );

        return r;
    }

    switch ( t->state )
    {
        // -----------------------------------
        case TPIU_UNSYNCED:
            return TPIU_EV_NONE;

        // -----------------------------------
        case TPIU_RXING:

            // We collect in sets of 16 bits, in order to filter halfsyncs (0x7fff)
            if ( !t->got_lowbits )
            {
                t->got_lowbits = true;
                t->rxedPacket[t->byteCount] = d;
                return TPIU_EV_NONE;
            }

            t->got_lowbits = false;

            if ( ( d == HALFSYNC_HIGH ) && ( t->rxedPacket[t->byteCount] == HALFSYNC_LOW ) )
            {
                // A halfsync, waste of space, to be ignored
                t->stats.halfSyncCount++;
                return TPIU_EV_NONE;
            }

            // Pre-increment for the low byte we already got, post increment for this one
            genericsReport( V_DEBUG, "[%02x %02x] ", t->rxedPacket[t->byteCount], d );
            t->byteCount++;
            t->rxedPacket[t->byteCount++] = d;

            if ( t->byteCount != TPIU_PACKET_LEN )
            {
                return TPIU_EV_RXING;
            }

            /* Check if this packet arrived a sensible time since the last one */
            gettimeofday( &nowTime, NULL );
            timersub( &nowTime, &t->lastPacket, &diffTime );
            memcpy( &t->lastPacket, &nowTime, sizeof( struct timeval ) );
            t->byteCount = 0;

            /* If it was less than a second since the last packet then it's valid */
            if ( diffTime.tv_sec < TIMEOUT )
            {
                t->stats.packets++;
                genericsReport( V_DEBUG, EOL );
                return TPIU_EV_RXEDPACKET;
            }
            else
            {
                genericsReport( V_WARN, ">>>>>>>>> PACKET INTERVAL TOO LONG <<<<<<<<<<<<<<" EOL );
                t->state = TPIU_UNSYNCED;
                t->stats.lostSync++;
                return TPIU_EV_UNSYNCED;
            }

        // -----------------------------------
        default:
            genericsReport( V_WARN, "In illegal state %d" EOL, t->state );
            t->stats.error++;
            return TPIU_EV_ERROR;
            // -----------------------------------
    }
}
// ====================================================================================================
