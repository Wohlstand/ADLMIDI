#include <vector>
#include <string>
#include <map>
#include <set>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <unistd.h>
#include <stdarg.h>
#include <cstdio>
#include <vector> // vector
#include <deque>  // deque
#include <cmath>  // exp, log, ceil

#include <SDL.h>
#include <deque>

#ifndef __MINGW32__
# include <pthread.h>
#endif

#include "dbopl.h"

static const unsigned long PCM_RATE = 48000;
static const unsigned MaxCards = 100;
static const unsigned MaxSamplesAtTime = 512; // dbopl limitation
static unsigned AdlBank    = 0;
static unsigned NumFourOps = 7;
static unsigned NumCards   = 2;

extern const struct adldata
{
    unsigned carrier_E862, modulator_E862;  // See below
    unsigned char carrier_40, modulator_40; // KSL/attenuation settings
    unsigned char feedconn; // Feedback/connection bits for the channel
    signed char finetune;
} adl[2887];
extern const struct adlinsdata
{
    unsigned short adlno1, adlno2;
    unsigned char tone;
    unsigned short ms_sound_kon;  // Number of milliseconds it produces sound;
    unsigned short ms_sound_koff;
} adlins[2811];
extern const unsigned short banks[48][256];
extern const char* const banknames[48];

static const unsigned short Operators[18] =
    {0x000,0x001,0x002, 0x008,0x009,0x00A, 0x010,0x011,0x012,
     0x100,0x101,0x102, 0x108,0x109,0x10A, 0x110,0x111,0x112 };
static const unsigned short Channels[18] =
    {0x000,0x001,0x002, 0x003,0x004,0x005, 0x006,0x007,0x008,
     0x100,0x101,0x102, 0x103,0x104,0x105, 0x106,0x107,0x108 };

struct OPL3
{
    unsigned NumChannels;

    std::vector<DBOPL::Handler> cards;
    std::vector<unsigned short> ins, pit, insmeta, midiins;

    std::vector<char> four_op_category; // 1 = master, 2 = slave, 0 = regular

    void Poke(unsigned card, unsigned index, unsigned value)
    {
        cards[card].WriteReg(index, value);
    }
    void NoteOff(unsigned c)
    {
        unsigned card = c/18, cc = c%18;
        Poke(card, 0xB0 + Channels[cc], pit[c] & 0xDF);
    }
    void NoteOn(unsigned c, double hertz) // Hertz range: 0..131071
    {
        unsigned card = c/18, cc = c%18;
        unsigned x = 0x2000;
        while(hertz >= 1023.5) { hertz /= 2.0; x += 0x400; } // Calculate octave
        x += (int)(hertz + 0.5);
        Poke(card, 0xA0 + Channels[cc], x & 0xFF);
        Poke(card, 0xB0 + Channels[cc], pit[c] = x >> 8);
    }
    void Touch_Real(unsigned c, unsigned volume)
    {
        unsigned card = c/18, cc = c%18;
        unsigned i = ins[c], o = Operators[cc];
        unsigned x = adl[i].carrier_40, y = adl[i].modulator_40;
        Poke(card, 0x40+o, (x|63) - volume + volume*(x&63)/63);
        Poke(card, 0x43+o, (y|63) - volume + volume*(y&63)/63);
        // Correct formula (ST3, AdPlug):
        //   63-((63-(instrvol))/63)*chanvol
        // Reduces to (tested identical):
        //   63 - chanvol + chanvol*instrvol/63
        // Also (slower, floats):
        //   63 + chanvol * (instrvol / 63.0 - 1)
    }
    void Touch(unsigned c, unsigned volume) // Volume maxes at 127*127*127
    {
        // The formula below: SOLVE(V=127^3 * 2^( (A-63.49999) / 8), A)
        Touch_Real(c, volume>8725  ? std::log(volume)*11.541561 + (0.5 - 104.22845) : 0);
        // The incorrect formula below: SOLVE(V=127^3 * (2^(A/63)-1), A)
        //Touch_Real(c, volume>11210 ? 91.61112 * std::log(4.8819E-7*volume + 1.0)+0.5 : 0);
    }
    void Patch(unsigned c, unsigned i)
    {
        unsigned card = c/18, cc = c%18;
        static const unsigned char data[4] = {0x20,0x60,0x80,0xE0};
        ins[c] = i;
        unsigned o1 = Operators[cc], o2 = o1 + 3;
        unsigned x = adl[i].carrier_E862, y = adl[i].modulator_E862;
        for(unsigned a=0; a<4; ++a)
        {
            Poke(card, data[a]+o1, x&0xFF); x>>=8;
            Poke(card, data[a]+o2, y&0xFF); y>>=8;
        }
    }
    void Pan(unsigned c, unsigned value)
    {
        unsigned card = c/18, cc = c%18;
        Poke(card, 0xC0 + Channels[cc], adl[ins[c]].feedconn | value);
    }
    void Silence() // Silence all OPL channels.
    {
        for(unsigned c=0; c<NumChannels; ++c) { NoteOff(c); Touch_Real(c,0); }
    }
    void Reset()
    {
        cards.resize(NumCards);
        NumChannels = NumCards * 18;
        ins.resize(NumChannels,     189);
        pit.resize(NumChannels,       0);
        insmeta.resize(NumChannels, 198);
        midiins.resize(NumChannels,   0);
        four_op_category.resize(NumChannels, 0);
        static const short data[] =
        { 0x004,96, 0x004,128,        // Pulse timer
          0x105, 0, 0x105,1, 0x105,0, // Pulse OPL3 enable
          0x001,32, 0x0BD,0x00,       // Enable wave, melodic mode
          0x105,1                     // Enable OPL3 extensions
        };
        unsigned fours = NumFourOps;
        for(unsigned card=0; card<NumCards; ++card)
        {
            cards[card].Init(PCM_RATE);
            for(unsigned a=0; a< sizeof(data)/sizeof(*data); a+=2)
                Poke(card, data[a], data[a+1]);
            unsigned fours_this_card = fours > 6 ? 6 : fours;
            Poke(card, 0x104, (1 << fours_this_card) - 1);
            //fprintf(stderr, "Card %u: %u four-ops.\n", card, fours_this_card);
            fours -= fours_this_card;
        }

        // Mark all channels that are reserved for four-operator function
        unsigned nextfour = 0;
        for(unsigned a=0; a<NumFourOps; ++a)
        {
            four_op_category[nextfour  ] = 1;
            four_op_category[nextfour+3] = 2;
            if(nextfour%3 == 2)
                nextfour += 9-2;
            else
                ++nextfour;
        }
        /*
        In two-op mode, channels 0..8 go as follows:
                      Op1[port]  Op2[port]
          Channel 0:  00  00     03  03
          Channel 1:  01  01     04  04
          Channel 2:  02  02     05  05
          Channel 3:  06  08     09  0B
          Channel 4:  07  09     10  0C
          Channel 5:  08  0A     11  0D
          Channel 6:  12  10     15  13
          Channel 7:  13  11     16  14
          Channel 8:  14  12     17  15
        In four-op mode, channels 0..8 go as follows:
                      Op1[port]  Op2[port]  Op3[port]  Op4[port]
          Channel 0:  00  00     03  03     06  08     09  0B
          Channel 1:  01  01     04  04     07  09     10  0C
          Channel 2:  02  02     05  05     08  0A     11  0D
          Channel 3:  CHANNEL 0 SLAVE
          Channel 4:  CHANNEL 1 SLAVE
          Channel 5:  CHANNEL 2 SLAVE
          Channel 6:  12  10     15  13
          Channel 7:  13  11     16  14
          Channel 8:  14  12     17  15
         Same goes principally for channels 9-17 respectively.
        */

        Silence();
    }
};

static const char MIDIsymbols[256+1] =
"PPPPPPhcckmvmxbd"
"oooooahoGGGGGGGG"
"BBBBBBBVVVVVHHMS"
"SSSOOOcTTTTTTTTX"
"XXXTTTFFFFFFFFFL"
"LLLLLLLppppppppX"
"XXXXXXXGGGGGTSSb"
"bbbMMMcGXXXXXXXD"
"????????????????"
"????????????????"
"???DDshMhhhCCCbM"
"CBDMMDDDMMDDDDDD"
"DDDDDDDDDDDDDD??"
"????????????????"
"????????????????"
"????????????????";

static class UI
{
public:
    int x, y, color, txtline;
    typedef char row[80];
    char slots[80][1 + 18*MaxCards], background[80][1 + 18*MaxCards];
    bool cursor_visible;
public:
    UI(): x(0), y(0), color(-1), txtline(1),
          cursor_visible(true)
        { std::fputc('\r', stderr); // Ensure cursor is at x=0
          std::memset(slots, '.',      sizeof(slots));
          std::memset(background, '.', sizeof(background));
        }
    void HideCursor()
    {
        if(!cursor_visible) return;
        cursor_visible = false;
        std::fprintf(stderr, "\33[?25l"); // hide cursor
    }
    void ShowCursor()
    {
        if(cursor_visible) return;
        cursor_visible = true;
        GotoXY(0,19); Color(7);
        std::fprintf(stderr, "\33[?25h"); // show cursor
        std::fflush(stderr);
    }
    void PrintLn(const char* fmt, ...) __attribute__((format(printf,2,3)))
    {
        va_list ap;
        va_start(ap, fmt);
        char Line[512];
      #ifndef __CYGWIN__
        int nchars = vsnprintf(Line, sizeof(Line), fmt, ap);
      #else
        int nchars = vsprintf(Line, fmt, ap); /* SECURITY: POSSIBLE BUFFER OVERFLOW */
      #endif
        va_end(ap);

        if(nchars == 0) return;

        const int beginx = 2;

        HideCursor();
        GotoXY(beginx,txtline);
        for(x=beginx; x-beginx<nchars && x < 80; ++x)
        {
            if(Line[x-beginx] == '\n') break;
            Color(Line[x-beginx] == '.' ? 1 : 8);
            std::fputc( background[x][txtline] = Line[x-beginx], stderr);
        }
        for(int tx=x; tx<80; ++tx)
        {
            if(background[tx][txtline]!='.' && slots[tx][txtline]=='.')
            {
                GotoXY(tx,txtline);
                Color(1);
                std::fputc(background[tx][txtline] = '.', stderr);
                ++x;
            }
        }
        std::fflush(stderr);

        txtline=1 + (txtline) % (18*NumCards);
    }
    void IllustrateNote(int adlchn, int note, int ins, int pressure, double bend)
    {
        HideCursor();
        int notex = 2 + (note+55)%77;
        int notey = 1+adlchn;
        char illustrate_char = background[notex][notey];
        if(pressure > 0)
        {
            illustrate_char = MIDIsymbols[ins];
            if(bend < 0) illustrate_char = '<';
            if(bend > 0) illustrate_char = '>';
        }
        else if(pressure < 0)
        {
            illustrate_char = '%';
        }
        if(slots[notex][notey] != illustrate_char)
        {
            slots[notex][notey] = illustrate_char;
            GotoXY(notex, notey);
            if(!pressure)
                Color(illustrate_char=='.' ? 1 : 8);
            else
                Color(  AllocateColor(ins) );
            std::fputc(illustrate_char, stderr);
            std::fflush(stderr);
            ++x;
        }
    }
    // Move tty cursor to the indicated position.
    // Movements will be done in relative terms
    // to the current cursor position only.
    void GotoXY(int newx, int newy)
    {
        while(newy > y) { std::fputc('\n', stderr); y+=1; x=0; }
        if(newy < y) { std::fprintf(stderr, "\33[%dA", y-newy); y = newy; }
        if(newx != x)
        {
            if(newx == 0 || (newx<10 && std::abs(newx-x)>=10))
                { std::fputc('\r', stderr); x = 0; }
            if(newx < x) std::fprintf(stderr, "\33[%dD", x-newx);
            if(newx > x) std::fprintf(stderr, "\33[%dC", newx-x);
            x = newx;
        }
    }
    // Set color (4-bit). Bits: 1=blue, 2=green, 4=red, 8=+intensity
    void Color(int newcolor)
    {
        if(color != newcolor)
        {
            static const char map[8+1] = "04261537";
            std::fprintf(stderr, "\33[0;%s40;3%c",
                (newcolor&8) ? "1;" : "", map[newcolor&7]);
            // If xterm-256color is used, try using improved colors:
            //        Translate 8 (dark gray) into #003366 (bluish dark cyan)
            //        Translate 1 (dark blue) into #000033 (darker blue)
            if(newcolor==8) std::fprintf(stderr, ";38;5;24;25");
            if(newcolor==1) std::fprintf(stderr, ";38;5;17;25");
            std::fputc('m', stderr);
            color=newcolor;
        }
    }
    // Choose a permanent color for given instrument
    int AllocateColor(int ins)
    {
        static char ins_colors[256] = { 0 }, ins_color_counter = 0;
        if(ins_colors[ins])
            return ins_colors[ins];
        if(ins & 0x80)
        {
            static const char shuffle[] = {2,3,4,5,6,7};
            return ins_colors[ins] = shuffle[ins_color_counter++ % 6];
        }
        else
        {
            static const char shuffle[] = {10,11,12,13,14,15};
            return ins_colors[ins] = shuffle[ins_color_counter++ % 6];
        }
    }
} UI;

class MIDIplay
{
    // Information about each track
    struct Position
    {
        bool began;
        double wait;
        struct TrackInfo
        {
            size_t ptr;
            long   delay;
            int    status;

            TrackInfo(): ptr(0), delay(0), status(0) { }
        };
        std::vector<TrackInfo> track;

        Position(): began(false), wait(0.0), track() { }
    } CurrentPosition, LoopBeginPosition;

    // Persistent settings for each MIDI channel
    struct MIDIchannel
    {
        unsigned short portamento;
        unsigned char bank_lsb, bank_msb;
        unsigned char patch;
        unsigned char volume, expression;
        unsigned char panning, vibrato, sustain;
        double bend, bendsense;
        double vibpos, vibspeed, vibdepth;
        long   vibdelay;
        unsigned char lastlrpn,lastmrpn; bool nrpn;
        struct NoteInfo
        {
            signed char adlchn1, adlchn2; // adlib channel
            unsigned char  vol;           // pressure
            unsigned short ins1, ins2;    // instrument selected on noteon
            unsigned short tone;          // tone selected for note
        };
        typedef std::map<unsigned char,NoteInfo> activenotemap_t;
        typedef activenotemap_t::iterator activenoteiterator;
        activenotemap_t activenotes;

        MIDIchannel()
            : portamento(0),
              bank_lsb(0), bank_msb(0), patch(0),
              volume(100),expression(100),
              panning(0x30), vibrato(0), sustain(0),
              bend(0.0), bendsense(2 / 8192.0),
              vibpos(0), vibspeed(2*3.141592653*5.0),
              vibdepth(0.5/127), vibdelay(0),
              lastlrpn(0),lastmrpn(0),nrpn(false),
              activenotes() { }
    } Ch[16];

    // Additional information about AdLib channels
    struct AdlChannel
    {
        // For collisions
        unsigned char midichn, note;
        // For channel allocation:
        enum { off, on, sustained } state;
        long age;
        AdlChannel(): midichn(0),note(0), state(off),age(0) { }
    };
    std::vector<AdlChannel> ch;
    std::vector< std::vector<unsigned char> > TrackData;
public:
    double InvDeltaTicks, Tempo;
    bool loopStart, loopEnd;
    OPL3 opl;
public:
    static unsigned long ReadBEInt(const void* buffer, unsigned nbytes)
    {
        unsigned long result=0;
        const unsigned char* data = (const unsigned char*) buffer;
        for(unsigned n=0; n<nbytes; ++n)
            result = (result << 8) + data[n];
        return result;
    }
    unsigned long ReadVarLen(unsigned tk)
    {
        unsigned long result = 0;
        for(;;)
        {
            unsigned char byte = TrackData[tk][CurrentPosition.track[tk].ptr++];
            result = (result << 7) + (byte & 0x7F);
            if(!(byte & 0x80)) break;
        }
        return result;
    }

    bool LoadMIDI(const std::string& filename)
    {
        FILE* fp = std::fopen(filename.c_str(), "rb");
        if(!fp) { std::perror(filename.c_str()); return false; }
        char HeaderBuf[4+4+2+2+2]="";
    riffskip:;
        std::fread(HeaderBuf, 1, 4+4+2+2+2, fp);
        if(std::memcmp(HeaderBuf, "RIFF", 4) == 0)
            { fseek(fp, 6, SEEK_CUR); goto riffskip; }
        if(std::memcmp(HeaderBuf, "MThd\0\0\0\6", 8) != 0)
        { InvFmt:
            std::fclose(fp);
            std::fprintf(stderr, "%s: Invalid format\n", filename.c_str());
            return false;
        }
        size_t Fmt        = ReadBEInt(HeaderBuf+8,  2);
        size_t TrackCount = ReadBEInt(HeaderBuf+10, 2);
        size_t DeltaTicks = ReadBEInt(HeaderBuf+12, 2);
        TrackData.resize(TrackCount);
        CurrentPosition.track.resize(TrackCount);
        InvDeltaTicks = 1e-6 / DeltaTicks;
        Tempo         = 1e6 * InvDeltaTicks;
        for(size_t tk = 0; tk < TrackCount; ++tk)
        {
            // Read track header
            std::fread(HeaderBuf, 1, 8, fp);
            if(std::memcmp(HeaderBuf, "MTrk", 4) != 0) goto InvFmt;
            size_t TrackLength = ReadBEInt(HeaderBuf+4, 4);
            // Read track data
            TrackData[tk].resize(TrackLength);
            std::fread(&TrackData[tk][0], 1, TrackLength, fp);
            // Read next event time
            CurrentPosition.track[tk].delay = ReadVarLen(tk);
        }
        loopStart = true;

        opl.Reset(); // Reset AdLib
        opl.Reset(); // ...twice (just in case someone misprogrammed OPL3 previously)
        ch.clear();
        ch.resize(opl.NumChannels);
        return true;
    }

    /* Periodic tick handler.
     *   Input: s           = seconds since last call
     *   Input: granularity = don't expect intervals smaller than this, in seconds
     *   Output: desired number of seconds until next call
     */
    double Tick(double s, double granularity)
    {
        if(CurrentPosition.began) CurrentPosition.wait -= s;
        while(CurrentPosition.wait <= granularity/2)
        {
            //std::fprintf(stderr, "wait = %g...\n", CurrentPosition.wait);
            ProcessEvents();
        }

        for(unsigned a=0; a<16; ++a)
            if(Ch[a].vibrato && !Ch[a].activenotes.empty())
            {
                NoteUpdate_All(a, Upd_Pitch);
                Ch[a].vibpos += s * Ch[a].vibspeed;
            }
            else
                Ch[a].vibpos = 0.0;

        return CurrentPosition.wait;
    }
private:
    enum { Upd_Patch  = 0x1,
           Upd_Pan    = 0x2,
           Upd_Volume = 0x4,
           Upd_Pitch  = 0x8,
           Upd_All    = Upd_Pan + Upd_Volume + Upd_Pitch,
           Upd_Off    = 0x20 };

    void NoteUpdate_Sub(
        int c,
        int tone,
        int ins,
        int vol,
        unsigned MidCh,
        unsigned props_mask)
    {
        if(c < 0) return;
        int midiins = opl.midiins[c];

        if(props_mask & Upd_Off) // note off
        {
            if(Ch[MidCh].sustain == 0)
            {
                opl.NoteOff(c);
                ch[c].age   = 0;
                ch[c].state = AdlChannel::off;
                UI.IllustrateNote(c, tone, midiins, 0, 0.0);
            }
            else
            {
                // Sustain: Forget about the note, but don't key it off.
                //          Also will avoid overwriting it very soon.
                ch[c].state = AdlChannel::sustained;
                UI.IllustrateNote(c, tone, midiins, -1, 0.0);
            }
        }
        if(props_mask & Upd_Patch)
        {
            opl.Patch(c, ins);
            ch[c].age = 0;
        }
        if(props_mask & Upd_Pan)
        {
            opl.Pan(c, Ch[MidCh].panning);
        }
        if(props_mask & Upd_Volume)
        {
            opl.Touch(c, vol * Ch[MidCh].volume * Ch[MidCh].expression);
        }
        if(props_mask & Upd_Pitch)
        {
            double bend = Ch[MidCh].bend + adl[ opl.ins[c] ].finetune;
            if(Ch[MidCh].vibrato && ch[c].age >= Ch[MidCh].vibdelay)
                bend += Ch[MidCh].vibrato * Ch[MidCh].vibdepth * std::sin(Ch[MidCh].vibpos);
            opl.NoteOn(c, 172.00093 * std::exp(0.057762265 * (tone + bend)));
            ch[c].state = AdlChannel::on;
            UI.IllustrateNote(c, tone, midiins, vol, Ch[MidCh].bend);
        }
    }

    void NoteUpdate
        (unsigned MidCh,
         MIDIchannel::activenoteiterator& i,
         unsigned props_mask)
     {
        NoteUpdate_Sub(
            i->second.adlchn1,
            i->second.tone,
            i->second.ins1,
            i->second.vol,
            MidCh,
            props_mask);

        NoteUpdate_Sub(
            i->second.adlchn2,
            i->second.tone,
            i->second.ins2,
            i->second.vol,
            MidCh,
            props_mask);

        if(props_mask & Upd_Off)
        {
            Ch[MidCh].activenotes.erase(i);
            i = Ch[MidCh].activenotes.end();
        }
    }

    void ProcessEvents()
    {
        loopEnd = false;
        const size_t TrackCount = TrackData.size();
        const Position RowBeginPosition ( CurrentPosition );
        for(size_t tk = 0; tk < TrackCount; ++tk)
        {
            if(CurrentPosition.track[tk].status >= 0
            && CurrentPosition.track[tk].delay <= 0)
            {
                // Handle event
                HandleEvent(tk);
                // Read next event time (unless the track just ended)
                if(CurrentPosition.track[tk].ptr >= TrackData[tk].size())
                    CurrentPosition.track[tk].status = -1;
                if(CurrentPosition.track[tk].status >= 0)
                    CurrentPosition.track[tk].delay += ReadVarLen(tk);
            }
        }
        // Find shortest delay from all track
        long shortest = -1;
        for(size_t tk=0; tk<TrackCount; ++tk)
            if(CurrentPosition.track[tk].status >= 0
            && (shortest == -1
               || CurrentPosition.track[tk].delay < shortest))
            {
                shortest = CurrentPosition.track[tk].delay;
            }
        //if(shortest > 0) UI.PrintLn("shortest: %ld", shortest);

        // Schedule the next playevent to be processed after that delay
        for(size_t tk=0; tk<TrackCount; ++tk)
            CurrentPosition.track[tk].delay -= shortest;

        double t = shortest * Tempo;
        if(CurrentPosition.began) CurrentPosition.wait += t;
        for(unsigned a = 0; a < opl.NumChannels; ++a)
            if(ch[a].age < 0x70000000)
                ch[a].age += t*1000;
        /*for(unsigned a=0; a < opl.NumChannels; ++a)
        {
            UI.GotoXY(64,a+1); UI.Color(2);
            std::fprintf(stderr, "%7ld,%c,%6ld\r",
                ch[a].age,
                "01s"[ch[a].state],
                ch[a].state == AdlChannel::off
                ? adlins[opl.insmeta[a]].ms_sound_koff
                : adlins[opl.insmeta[a]].ms_sound_kon);
            UI.x = 0;
        }*/

        //if(shortest > 0) UI.PrintLn("Delay %ld (%g)", shortest,t);

        if(loopStart)
        {
            LoopBeginPosition = RowBeginPosition;
            loopStart = false;
        }
        if(shortest < 0 || loopEnd)
        {
            // Loop if song end reached
            loopEnd         = false;
            CurrentPosition = LoopBeginPosition;
            shortest        = 0;
        }
    }
    void HandleEvent(size_t tk)
    {
        unsigned char byte = TrackData[tk][CurrentPosition.track[tk].ptr++];
        if(byte == 0xF7 || byte == 0xF0) // Ignore SysEx
        {
            unsigned length = ReadVarLen(tk);
            //std::string data( length?(const char*) &TrackData[tk][CurrentPosition.track[tk].ptr]:0, length );
            CurrentPosition.track[tk].ptr += length;
            UI.PrintLn("SysEx %02X: %u bytes", byte, length/*, data.c_str()*/);
            return;
        }
        if(byte == 0xFF)
        {
            // Special event FF
            unsigned char evtype = TrackData[tk][CurrentPosition.track[tk].ptr++];
            unsigned long length = ReadVarLen(tk);
            std::string data( length?(const char*) &TrackData[tk][CurrentPosition.track[tk].ptr]:0, length );
            CurrentPosition.track[tk].ptr += length;
            if(evtype == 0x2F) { CurrentPosition.track[tk].status = -1; return; }
            if(evtype == 0x51) { Tempo = ReadBEInt(data.data(), data.size()) * InvDeltaTicks; return; }
            if(evtype == 6 && data == "loopStart") loopStart = true;
            if(evtype == 6 && data == "loopEnd"  ) loopEnd   = true;
            if(evtype >= 1 && evtype <= 6)
                UI.PrintLn("Meta %d: %s", evtype, data.c_str());
            return;
        }
        // Any normal event (80..EF)
        if(byte < 0x80)
          { byte = CurrentPosition.track[tk].status | 0x80;
            CurrentPosition.track[tk].ptr--; }
        if(byte == 0xF3) { CurrentPosition.track[tk].ptr += 1; return; }
        if(byte == 0xF2) { CurrentPosition.track[tk].ptr += 2; return; }
        /*UI.PrintLn("@%X Track %u: %02X %02X",
            CurrentPosition.track[tk].ptr-1, (unsigned)tk, byte,
            TrackData[tk][CurrentPosition.track[tk].ptr]);*/
        unsigned MidCh = byte & 0x0F, EvType = byte >> 4;
        CurrentPosition.track[tk].status = byte;
        switch(EvType)
        {
            case 0x8: // Note off
            case 0x9: // Note on
            {
                int note = TrackData[tk][CurrentPosition.track[tk].ptr++];
                int  vol = TrackData[tk][CurrentPosition.track[tk].ptr++];
                NoteOff(MidCh, note);
                // On Note on, Keyoff the note first, just in case keyoff
                // was omitted; this fixes Dance of sugar-plum fairy
                // by Microsoft. Now that we've done a Keyoff,
                // check if we still need to do a Keyon.
                // vol=0 and event 8x are both Keyoff-only.
                if(vol == 0 || EvType == 0x8) break;

                unsigned midiins = Ch[MidCh].patch;
                if(MidCh == 9) midiins = 128 + note; // Percussion instrument

                static std::set<unsigned> bank_warnings;
                if(Ch[MidCh].bank_msb)
                {
                    unsigned bankid = midiins + 256*Ch[MidCh].bank_msb;
                    std::set<unsigned>::iterator
                        i = bank_warnings.lower_bound(bankid);
                    if(i == bank_warnings.end() || *i != bankid)
                    {
                        UI.PrintLn("[%u]Bank %u undefined, patch=%c%u",
                            MidCh,
                            Ch[MidCh].bank_msb,
                            (midiins&128)?'P':'M', midiins&127);
                        bank_warnings.insert(i, bankid);
                    }
                }
                if(Ch[MidCh].bank_lsb)
                {
                    unsigned bankid = Ch[MidCh].bank_lsb*65536;
                    std::set<unsigned>::iterator
                        i = bank_warnings.lower_bound(bankid);
                    if(i == bank_warnings.end() || *i != bankid)
                    {
                        UI.PrintLn("[%u]Bank lsb %u undefined",
                            MidCh,
                            Ch[MidCh].bank_lsb);
                        bank_warnings.insert(i, bankid);
                    }
                }

                int meta = banks[AdlBank][midiins];
                int tone = adlins[meta].tone ? adlins[meta].tone : note;
                int i[2] = { adlins[meta].adlno1, adlins[meta].adlno2 };

                // Allocate AdLib channel (the physical sound channel for the note)
                int adlchannel[2] = { -1, -1 };
                for(unsigned ccount = 0; ccount < 2; ++ccount)
                {
                    if(ccount == 1)
                    {
                        if(i[0] == i[1]) break; // No secondary channel
                        if(adlchannel[0] == -1) break; // No secondary if primary failed
                    }

                    int c = -1;
                    long bs = -adlins[meta].ms_sound_kon;
                    if(NumFourOps > 0) bs = -9999999;
                    for(int a = 0; a < (int)opl.NumChannels; ++a)
                    {
                        if(ccount == 1 && a == adlchannel[0]) continue;
                        // ^ Don't use the same channel for primary&secondary

                        if(i[0] == i[1])
                        {
                            // Only use regular channels
                            if(opl.four_op_category[a] != 0)
                                continue;
                        }
                        else
                        {
                            if(ccount == 0)
                            {
                                // Only use four-op master channels
                                if(opl.four_op_category[a] != 1)
                                    continue;
                            }
                            else
                            {
                                // The secondary must be played on a specific channel.
                                if(a != adlchannel[0] + 3)
                                    continue;
                            }
                        }

                        long s = ch[a].age;   // Age in seconds = better score
                        switch(ch[a].state)
                        {
                            case AdlChannel::on:
                            {
                                s -= adlins[opl.insmeta[a]].ms_sound_kon;
                                break;
                            }
                            case AdlChannel::sustained:
                            {
                                s -= adlins[opl.insmeta[a]].ms_sound_kon / 2;
                                break;
                            }
                            case AdlChannel::off:
                            {
                                s -= adlins[opl.insmeta[a]].ms_sound_koff / 2;
                                break;
                            }
                        }
                        if(i[ccount] == opl.ins[a]) s += 50;  // Same instrument = good
                        if(a == MidCh) s += 1;
                        s += 50 * (opl.midiins[a] / 128); // Percussion is inferior to melody
                        if(s > bs) { bs=s; c = a; } // Best candidate wins
                    }

                    if(c < 0)
                    {
                        //UI.PrintLn("ignored unplaceable note");
                        continue; // Could not play this note. Ignore it.
                    }
                    if(ch[c].state == AdlChannel::on)
                    {
                        /*UI.PrintLn(
                            "collision @%u: G%c%u[%ld/%ld] <- G%c%u",
                            c,
                            opl.midiins[c]<128?'M':'P', opl.midiins[c]&127,
                            ch[c].age, adlins[opl.insmeta[c]].ms_sound_kon,
                            midiins<128?'M':'P', midiins&127
                            );*/
                        NoteOff(ch[c].midichn, ch[c].note); // Collision: Kill old note
                    }
                    if(ch[c].state == AdlChannel::sustained)
                    {
                        NoteOffSustain(c);
                        // A sustained note needs to be keyoff'd
                        // first so that it can be retriggered.
                    }
                    adlchannel[ccount] = c;
                }
                if(adlchannel[0] < 0 && adlchannel[1] < 0)
                {
                    // The note could not be played, at all.
                    break;
                }
                //UI.PrintLn("i1=%d:%d, i2=%d:%d", i[0],adlchannel[0], i[1],adlchannel[1]);

                // Allocate active note for MIDI channel
                std::pair<MIDIchannel::activenoteiterator,bool>
                    ir = Ch[MidCh].activenotes.insert(
                        std::make_pair(note, MIDIchannel::NoteInfo()));
                ir.first->second.adlchn1 = adlchannel[0];
                ir.first->second.adlchn2 = adlchannel[1];
                ir.first->second.vol     = vol;
                ir.first->second.ins1    = i[0];
                ir.first->second.ins2    = i[1];
                ir.first->second.tone    = tone;
                for(unsigned ccount=0; ccount<2; ++ccount)
                {
                    int c = adlchannel[ccount];
                    if(c < 0) continue;
                    ch[c].midichn = MidCh;
                    ch[c].note    = note;
                    opl.insmeta[c] = meta;
                    opl.midiins[c] = midiins;
                }
                CurrentPosition.began  = true;
                NoteUpdate(MidCh, ir.first, Upd_All | Upd_Patch);
                break;
            }
            case 0xA: // Note touch
            {
                int note = TrackData[tk][CurrentPosition.track[tk].ptr++];
                int  vol = TrackData[tk][CurrentPosition.track[tk].ptr++];
                MIDIchannel::activenoteiterator
                    i = Ch[MidCh].activenotes.find(note);
                if(i == Ch[MidCh].activenotes.end())
                {
                    // Ignore touch if note is not active
                    break;
                }
                i->second.vol = vol;
                NoteUpdate(MidCh, i, Upd_Volume);
                break;
            }
            case 0xB: // Controller change
            {
                int ctrlno = TrackData[tk][CurrentPosition.track[tk].ptr++];
                int  value = TrackData[tk][CurrentPosition.track[tk].ptr++];
                switch(ctrlno)
                {
                    case 1: // Adjust vibrato
                        //UI.PrintLn("%u:vibrato %d", MidCh,value);
                        Ch[MidCh].vibrato = value; break;
                    case 0: // Set bank msb (GM bank)
                        Ch[MidCh].bank_msb = value;
                        break;
                    case 32: // Set bank lsb (XG bank)
                        Ch[MidCh].bank_lsb = value;
                        break;
                    case 5: // Set portamento msb
                        Ch[MidCh].portamento = (Ch[MidCh].portamento & 0x7F) | (value<<7);
                        UpdatePortamento(MidCh);
                        break;
                    case 37: // Set portamento lsb
                        Ch[MidCh].portamento = (Ch[MidCh].portamento & 0x3F80) | (value);
                        UpdatePortamento(MidCh);
                        break;
                    case 65: // Enable/disable portamento
                        // value >= 64 ? enabled : disabled
                        //UpdatePortamento(MidCh);
                        break;
                    case 7: // Change volume
                        Ch[MidCh].volume = value;
                        NoteUpdate_All(MidCh, Upd_Volume);
                        break;
                    case 64: // Enable/disable sustain
                        Ch[MidCh].sustain = value;
                        if(!value)
                            for(unsigned c = 0; c < opl.NumChannels; ++c)
                                if(ch[c].state == AdlChannel::sustained)
                                    NoteOffSustain(c);
                        break;
                    case 11: // Change expression (another volume factor)
                        Ch[MidCh].expression = value;
                        NoteUpdate_All(MidCh, Upd_Volume);
                        break;
                    case 10: // Change panning
                        Ch[MidCh].panning = 0x00;
                        if(value  < 64+32) Ch[MidCh].panning |= 0x10;
                        if(value >= 64-32) Ch[MidCh].panning |= 0x20;
                        NoteUpdate_All(MidCh, Upd_Pan);
                        break;
                    case 121: // Reset all controllers
                        Ch[MidCh].bend       = 0;
                        Ch[MidCh].volume     = 100;
                        Ch[MidCh].expression = 100;
                        Ch[MidCh].sustain    = 0;
                        Ch[MidCh].vibrato    = 0;
                        Ch[MidCh].vibspeed   = 2*3.141592653*5.0;
                        Ch[MidCh].vibdepth   = 0.5/127;
                        Ch[MidCh].vibdelay   = 0;
                        Ch[MidCh].panning    = 0x30;
                        Ch[MidCh].portamento = 0;
                        UpdatePortamento(MidCh);
                        NoteUpdate_All(MidCh, Upd_Pan+Upd_Volume+Upd_Pitch);
                        // Kill all sustained notes
                        for(unsigned c = 0; c < opl.NumChannels; ++c)
                            if(ch[c].state == AdlChannel::sustained)
                                NoteOffSustain(c);
                        break;
                    case 123: // All notes off
                        NoteUpdate_All(MidCh, Upd_Off);
                        break;
                    case 91: break; // Reverb effect depth. We don't do per-channel reverb.
                    case 92: break; // Tremolo effect depth. We don't do...
                    case 93: break; // Chorus effect depth. We don't do.
                    case 94: break; // Celeste effect depth. We don't do.
                    case 95: break; // Phaser effect depth. We don't do.
                    case 98: Ch[MidCh].lastlrpn=value; Ch[MidCh].nrpn=true; break;
                    case 99: Ch[MidCh].lastmrpn=value; Ch[MidCh].nrpn=true; break;
                    case 100:Ch[MidCh].lastlrpn=value; Ch[MidCh].nrpn=false; break;
                    case 101:Ch[MidCh].lastmrpn=value; Ch[MidCh].nrpn=false; break;
                    case 113: break; // Related to pitch-bender, used by missimp.mid in Duke3D
                    case  6: SetRPN(MidCh, value, true); break;
                    case 38: SetRPN(MidCh, value, false); break;
                    default:
                        UI.PrintLn("Ctrl %d <- %d (ch %u)", ctrlno, value, MidCh);
                }
                break;
            }
            case 0xC: // Patch change
                Ch[MidCh].patch = TrackData[tk][CurrentPosition.track[tk].ptr++];
                break;
            case 0xD: // Channel after-touch
            {
                // TODO: Verify, is this correct action?
                int  vol = TrackData[tk][CurrentPosition.track[tk].ptr++];
                for(MIDIchannel::activenoteiterator
                    i = Ch[MidCh].activenotes.begin();
                    i != Ch[MidCh].activenotes.end();
                    ++i)
                {
                    // Set this pressure to all active notes on the channel
                    i->second.vol = vol;
                }
                NoteUpdate_All(MidCh, Upd_Volume);
                break;
            }
            case 0xE: // Wheel/pitch bend
            {
                int a = TrackData[tk][CurrentPosition.track[tk].ptr++];
                int b = TrackData[tk][CurrentPosition.track[tk].ptr++];
                Ch[MidCh].bend = (a + b*128 - 8192) * Ch[MidCh].bendsense;
                NoteUpdate_All(MidCh, Upd_Pitch);
                break;
            }
        }
    }

    void SetRPN(unsigned MidCh, unsigned value, bool MSB)
    {
        bool nrpn = Ch[MidCh].nrpn;
        unsigned addr = Ch[MidCh].lastmrpn*0x100 + Ch[MidCh].lastlrpn;
        switch(addr + nrpn*0x10000 + MSB*0x20000)
        {
            case 0x0000 + 0*0x10000 + 1*0x20000: // Pitch-bender sensitivity
                Ch[MidCh].bendsense = value/8192.0;
                break;
            case 0x0108 + 1*0x10000 + 1*0x20000: // Vibrato speed
                if(value == 64)
                    Ch[MidCh].vibspeed = 1.0;
                else if(value < 100)
                    Ch[MidCh].vibspeed = 1.0/(1.6e-2*(value?value:1));
                else
                    Ch[MidCh].vibspeed = 1.0/(0.051153846*value-3.4965385);
                Ch[MidCh].vibspeed *= 2*3.141592653*5.0;
                break;
            case 0x0109 + 1*0x10000 + 1*0x20000: // Vibrato depth
                Ch[MidCh].vibdepth = ((value-64)*0.15)*0.01;
                break;
            case 0x010A + 1*0x10000 + 1*0x20000: // Vibrato delay in millisecons
                Ch[MidCh].vibdelay =
                    value ? long(0.2092 * std::exp(0.0795 * value)) : 0.0;
                break;
            default: UI.PrintLn("%s %04X <- %d (%cSB) (ch %u)",
                "NRPN"+!nrpn, addr, value, "LM"[MSB], MidCh);
        }
    }

    void UpdatePortamento(unsigned MidCh)
    {
        // mt = 2^(portamento/2048) * (1.0 / 5000.0)
        /*
        double mt = std::exp(0.00033845077 * Ch[MidCh].portamento);
        NoteUpdate_All(MidCh, Upd_Pitch);
        */
        UI.PrintLn("Portamento %u: %u (unimplemented)", MidCh, Ch[MidCh].portamento);
    }

    void NoteUpdate_All(unsigned MidCh, unsigned props_mask)
    {
        for(MIDIchannel::activenoteiterator
            i = Ch[MidCh].activenotes.begin();
            i != Ch[MidCh].activenotes.end();
            )
        {
            MIDIchannel::activenoteiterator j(i++);
            NoteUpdate(MidCh, j, props_mask);
        }
    }
    void NoteOff(unsigned MidCh, int note)
    {
        MIDIchannel::activenoteiterator
            i = Ch[MidCh].activenotes.find(note);
        if(i != Ch[MidCh].activenotes.end())
        {
            NoteUpdate(MidCh, i, Upd_Off);
        }
    }
    void NoteOffSustain(unsigned c)
    {
        UI.IllustrateNote(c, ch[c].note, opl.midiins[c], 0, 0.0);
        opl.NoteOff(c);
        ch[c].state = AdlChannel::off;
    }
};

struct Reverb /* This reverb implementation is based on Freeverb impl. in Sox */
{
    float feedback, hf_damping, gain;
    struct FilterArray
    {
        struct Filter
        {
            std::vector<float> Ptr;  size_t pos;  float Store;
            void Create(size_t size) { Ptr.resize(size); pos = 0; Store = 0.f; }
            float Update(float a, float b)
            {
                Ptr[pos] = a;
                if(!pos) pos = Ptr.size()-1; else --pos;
                return b;
            }
            float ProcessComb(float input, const float feedback, const float hf_damping)
            {
                Store = Ptr[pos] + (Store - Ptr[pos]) * hf_damping;
                return Update(input + feedback * Store, Ptr[pos]);
            }
            float ProcessAllPass(float input)
            {
                return Update(input + Ptr[pos] * .5f, Ptr[pos]-input);
            }
        } comb[8], allpass[4];
        void Create(double rate, double scale, double offset)
        {
            /* Filter delay lengths in samples (44100Hz sample-rate) */
            static const int comb_lengths[8] = {1116,1188,1277,1356,1422,1491,1557,1617};
            static const int allpass_lengths[4] = {225,341,441,556};
            double r = rate * (1 / 44100.0); // Compensate for actual sample-rate
            const int stereo_adjust = 12;
            for(size_t i=0; i<8; ++i, offset=-offset)
                comb[i].Create( scale * r * (comb_lengths[i] + stereo_adjust * offset) + .5 );
            for(size_t i=0; i<4; ++i, offset=-offset)
                allpass[i].Create( r * (allpass_lengths[i] + stereo_adjust * offset) + .5 );
        }
        void Process(size_t length,
            const std::deque<float>& input, std::vector<float>& output,
            const float feedback, const float hf_damping, const float gain)
        {
            for(size_t a=0; a<length; ++a)
            {
                float out = 0, in = input[a];
                for(size_t i=8; i-- > 0; ) out += comb[i].ProcessComb(in, feedback, hf_damping);
                for(size_t i=4; i-- > 0; ) out += allpass[i].ProcessAllPass(out);
                output[a] = out * gain;
            }
        }
    } chan[2];
    std::vector<float> out[2];
    std::deque<float> input_fifo;

    void Create(double sample_rate_Hz,
        double wet_gain_dB,
        double room_scale, double reverberance, double fhf_damping, /* 0..1 */
        double pre_delay_s, double stereo_depth,
        size_t buffer_size)
    {
        size_t delay = pre_delay_s  * sample_rate_Hz + .5;
        double scale = room_scale * .9 + .1;
        double depth = stereo_depth;
        double a =  -1 /  std::log(1 - /**/.3 /**/);          // Set minimum feedback
        double b = 100 / (std::log(1 - /**/.98/**/) * a + 1); // Set maximum feedback
        feedback = 1 - std::exp((reverberance*100.0 - b) / (a * b));
        hf_damping = fhf_damping * .3 + .2;
        gain = std::exp(wet_gain_dB * (std::log(10.0) * 0.05)) * .015;
        input_fifo.insert(input_fifo.end(), delay, 0.f);
        for(size_t i = 0; i <= std::ceil(depth); ++i)
        {
            chan[i].Create(sample_rate_Hz, scale, i * depth);
            out[i].resize(buffer_size);
        }
    }
    void Process(size_t length)
    {
        for(size_t i=0; i<2; ++i)
            if(!out[i].empty())
                chan[i].Process(length,
                    input_fifo,
                    out[i], feedback, hf_damping, gain);
        input_fifo.erase(input_fifo.begin(), input_fifo.begin() + length);
    }
};
static struct MyReverbData
{
    bool wetonly;
    Reverb chan[2];

    MyReverbData() : wetonly(false)
    {
        for(size_t i=0; i<2; ++i)
            chan[i].Create(PCM_RATE,
                4.0,  // wet_gain_dB  (-10..10)
                .8,//.7,   // room_scale   (0..1)
                0.,//.6,   // reverberance (0..1)
                .5,   // hf_damping   (0..1)
                .000, // pre_delay_s  (0.. 0.5)
                .6,   // stereo_depth (0..1)
                MaxSamplesAtTime);
    }
} reverb_data;


static std::deque<short> AudioBuffer;
#ifndef __MINGW32__
static pthread_mutex_t AudioBuffer_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

static void SDL_AudioCallback(void*, Uint8* stream, int len)
{
    SDL_LockAudio();
    short* target = (short*) stream;
    #ifndef __MINGW32__
    pthread_mutex_lock(&AudioBuffer_lock);
    #endif
    /*if(len != AudioBuffer.size())
        fprintf(stderr, "len=%d stereo samples, AudioBuffer has %u stereo samples",
            len/4, (unsigned) AudioBuffer.size()/2);*/
    unsigned ate = len/2; // number of shorts
    if(ate > AudioBuffer.size()) ate = AudioBuffer.size();
    for(unsigned a=0; a<ate; ++a)
        target[a] = AudioBuffer[a];
    AudioBuffer.erase(AudioBuffer.begin(), AudioBuffer.begin() + ate);
    //fprintf(stderr, " - remain %u\n", (unsigned) AudioBuffer.size()/2);
    #ifndef __MINGW32__
    pthread_mutex_unlock(&AudioBuffer_lock);
    #endif
    SDL_UnlockAudio();
}
static void SendStereoAudio(unsigned long count, int* samples)
{
    if(!count) return;

    // Attempt to filter out the DC component. However, avoid doing
    // sudden changes to the offset, for it can be audible.
    double average[2]={0,0};
    for(unsigned w=0; w<2; ++w)
        for(unsigned long p = 0; p < count; ++p)
            average[w] += samples[p*2+w];
    static float prev_avg_flt[2] = {0,0};
    float average_flt[2] =
    {
        prev_avg_flt[0] = (prev_avg_flt[0] + average[0]*0.04/double(count)) / 1.04,
        prev_avg_flt[1] = (prev_avg_flt[1] + average[1]*0.04/double(count)) / 1.04
    };
    // Figure out the amplitude of both channels
    static unsigned amplitude_display_counter = 0;
    if(!amplitude_display_counter--)
    {
        amplitude_display_counter = (PCM_RATE / count) / 24;
        double amp[2]={0,0};
        const unsigned maxy = NumCards*18;
        for(unsigned w=0; w<2; ++w)
        {
            average[w] /= double(count);
            for(unsigned long p = 0; p < count; ++p)
                amp[w] += std::fabs(samples[p*2+w] - average[w]);
            amp[w] /= double(count);
            // Turn into logarithmic scale
            const double dB = std::log(amp[w]<1 ? 1 : amp[w]) * 4.328085123;
            const double maxdB = 3*16; // = 3 * log2(65536)
            amp[w] = maxy*dB/maxdB;
        }
        const unsigned white_threshold  = maxy/18;
        const unsigned red_threshold    = maxy*4/18;
        const unsigned yellow_threshold = maxy*8/18;
        for(unsigned y=0; y<maxy; ++y)
            for(unsigned w=0; w<2; ++w)
            {
                char c = amp[w] > (maxy-1)-y ? '|' : UI.background[w][y+1];
                if(UI.slots[w][y+1] != c)
                {
                    UI.slots[w][y+1] = c;
                    UI.HideCursor();
                    UI.GotoXY(w,y+1);
                    UI.Color(c=='|' ? y<white_threshold ? 15
                                    : y<red_threshold ? 12
                                    : y<yellow_threshold ? 14
                                    : 10 :
                            (c=='.' ? 1 : 8));
                    std::fputc(c, stderr);
                    UI.x += 1;
    }       }   }

    // Convert input to float format
    std::vector<float> dry[2];
    for(unsigned w=0; w<2; ++w)
    {
        dry[w].resize(count);
        for(unsigned long p = 0; p < count; ++p)
            dry[w][p] = (samples[p*2+w] - average_flt[w]) * double(0.6/32768.0);
        reverb_data.chan[w].input_fifo.insert(
        reverb_data.chan[w].input_fifo.end(),
            dry[w].begin(), dry[w].end());
    }
    // Reverbify it
    for(unsigned w=0; w<2; ++w)
        reverb_data.chan[w].Process(count);

    // Convert to signed 16-bit int format and put to playback queue
    #ifndef __MINGW32__
    pthread_mutex_lock(&AudioBuffer_lock);
    #endif
    size_t pos = AudioBuffer.size();
    AudioBuffer.resize(pos + count*2);
    for(unsigned long p = 0; p < count; ++p)
        for(unsigned w=0; w<2; ++w)
        {
            float out = ((1 - reverb_data.wetonly) * dry[w][p] +
                .5 * (reverb_data.chan[0].out[w][p]
                    + reverb_data.chan[1].out[w][p])) * 32768.0f
                 + average_flt[w];
            AudioBuffer[pos+p*2+w] =
                out<-32768.f ? -32768 :
                out>32767.f ?  32767 : out;
        }
    #ifndef __MINGW32__
    pthread_mutex_unlock(&AudioBuffer_lock);
    #endif
}

int main(int argc, char** argv)
{
    const unsigned Interval = 50;
    static SDL_AudioSpec spec;
    spec.freq     = PCM_RATE;
    spec.format   = AUDIO_S16SYS;
    spec.channels = 2;
    spec.samples  = spec.freq / Interval;
    spec.callback = SDL_AudioCallback;
    if(SDL_OpenAudio(&spec, 0) < 0)
    {
        std::fprintf(stderr, "Couldn't open audio: %s\n", SDL_GetError());
        return 1;
    }

    if(argc < 2)
    {
        std::printf(
            "Usage: midiplay <midifilename> [ <banknumber> [ <numcards> [ <numfourops>] ] ]\n");
        for(unsigned a=0; a<sizeof(banknames)/sizeof(*banknames); ++a)
            std::printf("%10s%2u = %s\n",
                a?"":"Banks:",
                a,
                banknames[a]);
        std::printf(
            "     Use banks 1-4 to play Descent \"q\" soundtracks.\n"
            "     Look up the relevant bank number from descent.sng.\n"
            "\n"
            "     The fourth parameter can be used to specify the number\n"
            "     of four-op channels to use. Each four-op channel eats\n"
            "     the room of two regular channels. Use as many as required.\n"
            "     The Doom & Hexen sets require one or two, while\n"
            "     Miles four-op set requires the maximum of numcards*6.\n"
            "\n"
            );
        return 0;
    }

    if(argc >= 3)
    {
        const unsigned NumBanks = sizeof(banknames)/sizeof(*banknames);
        AdlBank = std::atoi(argv[2]);
        if(AdlBank >= NumBanks)
        {
            std::fprintf(stderr, "bank number may only be 0..%u.\n", NumBanks-1);
            return 0;
        }
        std::printf("FM instrument bank %u selected.\n", AdlBank);
    }

    unsigned n_fourop[2] = {0,0}, n_total[2] = {0,0};
    for(unsigned a=0; a<256; ++a)
    {
        unsigned insno = banks[AdlBank][a];
        if(insno == 198) continue;
        ++n_total[a/128];
        if(adlins[insno].adlno1 != adlins[insno].adlno2)
            ++n_fourop[a/128];
    }
    std::printf("This bank has %u/%u four-op melodic instruments and %u/%u percussive ones.\n",
        n_fourop[0], n_total[0],
        n_fourop[1], n_total[1]);

    if(argc >= 4)
    {
        NumCards = std::atoi(argv[3]);
        if(NumCards < 1 || NumCards > MaxCards)
        {
            std::fprintf(stderr, "number of cards may only be 1..%u.\n", MaxCards);
            return 0;
        }
    }
    if(argc >= 5)
    {
        NumFourOps = std::atoi(argv[4]);
        if(NumFourOps > 6 * NumCards)
        {
            std::fprintf(stderr, "number of four-op channels may only be 0..%u when %u OPL3 cards are used.\n",
                6*NumCards, NumCards);
            return 0;
        }
    }
    else
        NumFourOps =
            (n_fourop[0] >= n_total[0]*7/8) ? NumCards * 6
          : (n_fourop[0] < n_total[0]*1/8) ? 0
          : (NumCards==1 ? 1 : (5 + (NumCards-1)*3));

    std::printf(
        "Simulating %u OPL3 cards for a total of %u operators.\n"
        "Setting up the operators as %u four-op channels, %u dual-op channels.\n",
        NumCards, NumCards*36,
        NumFourOps, 18*NumCards - NumFourOps*2);

    MIDIplay player;
    if(!player.LoadMIDI(argv[1]))
        return 2;

    if(n_fourop[0] >= n_total[0]*15/16 && NumFourOps == 0)
    {
        std::fprintf(stderr,
            "ERROR: You have selected a bank that consists almost exclusively of four-op patches.\n"
            "       The results (silence + much cpu load) would be probably\n"
            "       not what you want, therefore ignoring the request.\n");
        return 0;
    }

    SDL_PauseAudio(0);

    const double mindelay = 1 / (double)PCM_RATE;
    const double maxdelay = MaxSamplesAtTime / (double)PCM_RATE;

    UI.GotoXY(0,0); UI.Color(15);
    std::fprintf(stderr, "Hit Ctrl-C to quit\r");

    for(double delay=0; ; )
    {
        const double eat_delay = delay < maxdelay ? delay : maxdelay;
        delay -= eat_delay;

        static double carry = 0.0;
        carry += PCM_RATE * eat_delay;
        const unsigned long n_samples = (unsigned) carry;
        carry -= n_samples;

        if(NumCards == 1)
        {
            player.opl.cards[0].Generate(0, SendStereoAudio, n_samples);
        }
        else
        {
            /* Mix together the audio from different cards */
            static std::vector<int> sample_buf;
            sample_buf.clear();
            sample_buf.resize(n_samples*2);
            struct Mix
            {
                static void AddStereoAudio(unsigned long count, int* samples)
                {
                    for(unsigned long a=0; a<count*2; ++a)
                        sample_buf[a] += samples[a];
                }
            };
            for(unsigned card = 0; card < NumCards; ++card)
            {
                player.opl.cards[card].Generate(
                    0,
                    Mix::AddStereoAudio,
                    n_samples);
            }
            /* Process it */
            SendStereoAudio(n_samples, &sample_buf[0]);
        }

        while(AudioBuffer.size() > 3*spec.freq/Interval)
            SDL_Delay(1e3 * eat_delay);

        double nextdelay = player.Tick(eat_delay, mindelay);
        UI.ShowCursor();

        delay = nextdelay;
    }

    SDL_CloseAudio();
    return 0;
}

/* THIS ADLIB FM INSTRUMENT DATA IS AUTOMATICALLY GENERATED FROM
 * THE FOLLOWING SOURCES:
 *    MILES SOUND SYSTEM FOR STAR CONTROL III
 *    DESCENT DATA FILES (HUMAN MACHINE INTERFACES & PARALLAX SOFTWARE)
 *    DOOM SOUND DRIVER
 *    HEXEN/HERETIC SOUND DRIVER
 *    MILES SOUND SYSTEM FOR WARCRAFT 2
 *    MILES SOUND SYSTEM FOR SIM FARM (QUAD OPERATOR MODE)
 *    MILES SOUND SYSTEM FOR SIM FARM
 * PREPROCESSED, CONVERTED, AND POSTPROCESSED OFF-SCREEN.
 */
const adldata adl[2887] =
{ //    ,---------+-------- Wave select settings
  //    | ,-------�-+------ Sustain/release rates
  //    | | ,-----�-�-+---- Attack/decay rates
  //    | | | ,---�-�-�-+-- AM/VIB/EG/KSR/Multiple bits
  //    | | | |   | | | |
  //    | | | |   | | | |     ,----+-- KSL/attenuation settings
  //    | | | |   | | | |     |    |    ,----- Feedback/connection bits
  //    | | | |   | | | |     |    |    |
    { 0x0F4F201,0x0F7F201, 0x8F,0x06, 0x8,+0 }, // 0: GM0; b45M0; f29GM0; f30GM0; sGM0; AcouGrandPiano; am000
    { 0x0F4F201,0x0F7F201, 0x4B,0x00, 0x8,+0 }, // 1: GM1; b45M1; f29GM1; f30GM1; sGM1; BrightAcouGrand; am001
    { 0x0F4F201,0x0F6F201, 0x49,0x00, 0x8,+0 }, // 2: GM2; b45M2; f29GM2; f30GM2; f34GM0; f34GM1; f34GM2; sGM2; AcouGrandPiano; BrightAcouGrand; ElecGrandPiano; am002
    { 0x0F7F281,0x0F7F241, 0x12,0x00, 0x6,+0 }, // 3: GM3; b45M3; f34GM3; sGM3; Honky-tonkPiano; am003
    { 0x0F7F101,0x0F7F201, 0x57,0x00, 0x0,+0 }, // 4: GM4; b45M4; f34GM4; sGM4; Rhodes Piano; am004
    { 0x0F7F101,0x0F7F201, 0x93,0x00, 0x0,+0 }, // 5: GM5; b45M5; f29GM6; f30GM6; f34GM5; sGM5; Chorused Piano; Harpsichord; am005
    { 0x0F2A101,0x0F5F216, 0x80,0x0E, 0x8,+0 }, // 6: GM6; b45M6; f34GM6; Harpsichord; am006
    { 0x0F8C201,0x0F8C201, 0x92,0x00, 0xA,+0 }, // 7: GM7; b45M7; f34GM7; sGM7; Clavinet; am007
    { 0x0F4F60C,0x0F5F381, 0x5C,0x00, 0x0,+0 }, // 8: GM8; b45M8; f34GM8; sGM8; Celesta; am008
    { 0x0F2F307,0x0F1F211, 0x97,0x80, 0x2,+0 }, // 9: GM9; b45M9; f29GM101; f30GM101; f34GM9; FX 6 goblins; Glockenspiel; am009
    { 0x0F45417,0x0F4F401, 0x21,0x00, 0x2,+0 }, // 10: GM10; b45M10; f29GM100; f30GM100; f34GM10; sGM10; FX 5 brightness; Music box; am010
    { 0x0F6F398,0x0F6F281, 0x62,0x00, 0x0,+0 }, // 11: GM11; b45M11; f34GM11; sGM11; Vibraphone; am011
    { 0x0F6F618,0x0F7E701, 0x23,0x00, 0x0,+0 }, // 12: GM12; b45M12; f29GM104; f29GM97; f30GM104; f30GM97; f34GM12; sGM12; FX 2 soundtrack; Marimba; Sitar; am012
    { 0x0F6F615,0x0F6F601, 0x91,0x00, 0x4,+0 }, // 13: GM13; b45M13; f29GM103; f30GM103; f34GM13; sGM13; FX 8 sci-fi; Xylophone; am013
    { 0x0F3D345,0x0F3A381, 0x59,0x80, 0xC,+0 }, // 14: GM14; b45M14; f34GM14; Tubular Bells; am014
    { 0x1F57503,0x0F5B581, 0x49,0x80, 0x4,+0 }, // 15: GM15; b45M15; f34GM15; sGM15; Dulcimer; am015
    { 0x014F671,0x007F131, 0x92,0x00, 0x2,+0 }, // 16: GM16; HMIGM16; b45M16; f34GM16; sGM16; Hammond Organ; am016; am016.in
    { 0x058C772,0x008C730, 0x14,0x00, 0x2,+0 }, // 17: GM17; HMIGM17; b45M17; f34GM17; sGM17; Percussive Organ; am017; am017.in
    { 0x018AA70,0x0088AB1, 0x44,0x00, 0x4,+0 }, // 18: GM18; HMIGM18; b45M18; f34GM18; sGM18; Rock Organ; am018; am018.in
    { 0x1239723,0x01455B1, 0x93,0x00, 0x4,+0 }, // 19: GM19; HMIGM19; b45M19; f34GM19; Church Organ; am019; am019.in
    { 0x1049761,0x00455B1, 0x13,0x80, 0x0,+0 }, // 20: GM20; HMIGM20; b45M20; f34GM20; sGM20; Reed Organ; am020; am020.in
    { 0x12A9824,0x01A46B1, 0x48,0x00, 0xC,+0 }, // 21: GM21; HMIGM21; b45M21; f34GM21; sGM21; Accordion; am021; am021.in
    { 0x1069161,0x0076121, 0x13,0x00, 0xA,+0 }, // 22: GM22; HMIGM22; b45M22; f34GM22; sGM22; Harmonica; am022; am022.in
    { 0x0067121,0x00761A1, 0x13,0x89, 0x6,+0 }, // 23: GM23; HMIGM23; b45M23; f34GM23; sGM23; Tango Accordion; am023; am023.in
    { 0x194F302,0x0C8F341, 0x9C,0x80, 0xC,+0 }, // 24: GM24; HMIGM24; b45M24; f34GM24; Acoustic Guitar1; am024; am024.in
    { 0x19AF303,0x0E7F111, 0x54,0x00, 0xC,+0 }, // 25: GM25; HMIGM25; b45M25; f17GM25; f29GM60; f30GM60; f34GM25; mGM25; sGM25; Acoustic Guitar2; French Horn; am025; am025.in
    { 0x03AF123,0x0F8F221, 0x5F,0x00, 0x0,+0 }, // 26: GM26; HMIGM26; b45M26; f17GM26; f34GM26; f35GM26; mGM26; sGM26; Electric Guitar1; am026; am026.in
    { 0x122F603,0x0F8F321, 0x87,0x80, 0x6,+0 }, // 27: GM27; b45M27; f30GM61; f34GM27; sGM27; Brass Section; Electric Guitar2; am027
    { 0x054F903,0x03AF621, 0x47,0x00, 0x0,+0 }, // 28: GM28; HMIGM28; b45M28; f17GM28; f34GM28; f35GM28; hamM3; hamM60; intM3; mGM28; rickM3; sGM28; BPerc; BPerc.in; Electric Guitar3; am028; am028.in; muteguit
    { 0x1419123,0x0198421, 0x4A,0x05, 0x8,+0 }, // 29: GM29; b45M29; f34GM29; sGM29; Overdrive Guitar; am029
    { 0x1199523,0x0199421, 0x4A,0x00, 0x8,+0 }, // 30: GM30; HMIGM30; b45M30; f17GM30; f34GM30; f35GM30; hamM6; intM6; mGM30; rickM6; sGM30; Distorton Guitar; GDist; GDist.in; am030; am030.in
    { 0x04F2009,0x0F8D184, 0xA1,0x80, 0x8,+0 }, // 31: GM31; HMIGM31; b45M31; f34GM31; hamM5; intM5; rickM5; sGM31; GFeedbck; Guitar Harmonics; am031; am031.in
    { 0x0069421,0x0A6C3A2, 0x1E,0x00, 0x2,+0 }, // 32: GM32; HMIGM32; b45M32; f34GM32; sGM32; Acoustic Bass; am032; am032.in
    { 0x028F131,0x018F131, 0x12,0x00, 0xA,+0 }, // 33: GM33; GM39; HMIGM33; HMIGM39; b45M33; b45M39; f15GM30; f17GM33; f17GM39; f26GM30; f29GM28; f29GM29; f30GM28; f30GM29; f34GM33; f34GM39; f35GM39; hamM68; mGM33; mGM39; sGM33; sGM39; Distorton Guitar; Electric Bass 1; Electric Guitar3; Overdrive Guitar; Synth Bass 2; am033; am033.in; am039; am039.in; synbass2
    { 0x0E8F131,0x078F131, 0x8D,0x00, 0xA,+0 }, // 34: GM34; HMIGM34; b45M34; f15GM28; f17GM34; f26GM28; f29GM38; f29GM67; f30GM38; f30GM67; f34GM34; f35GM37; mGM34; rickM81; sGM34; Baritone Sax; Electric Bass 2; Electric Guitar3; Slap Bass 2; Slapbass; Synth Bass 1; am034; am034.in
    { 0x0285131,0x0487132, 0x5B,0x00, 0xC,+0 }, // 35: GM35; HMIGM35; b45M35; b46M35; b47M35; f17GM35; f20GM35; f29GM42; f29GM70; f29GM71; f30GM42; f30GM70; f30GM71; f31GM35; f34GM35; f36GM35; f49GM35; mGM35; qGM35; sGM35; Bassoon; Cello; Clarinet; Fretless Bass; am035; am035.in; gm035
    { 0x09AA101,0x0DFF221, 0x8B,0x40, 0x8,+0 }, // 36: GM36; HMIGM36; b45M36; b46M36; b47M36; f17GM36; f20GM36; f29GM68; f30GM68; f31GM36; f34GM36; f36GM36; f49GM36; mGM36; qGM36; sGM36; Oboe; Slap Bass 1; am036; am036.in; gm036
    { 0x016A221,0x0DFA121, 0x8B,0x08, 0x8,+0 }, // 37: GM37; b45M37; f29GM69; f30GM69; f34GM37; sGM37; English Horn; Slap Bass 2; am037
    { 0x0E8F431,0x078F131, 0x8B,0x00, 0xA,+0 }, // 38: GM38; HMIGM38; b45M38; f17GM38; f29GM30; f29GM31; f30GM30; f30GM31; f34GM38; f35GM38; hamM13; hamM67; intM13; mGM38; rickM13; sGM38; BSynth3; BSynth3.; Distorton Guitar; Guitar Harmonics; Synth Bass 1; am038; am038.in; synbass1
    { 0x113DD31,0x0265621, 0x15,0x00, 0x8,+0 }, // 39: GM40; HMIGM40; b45M40; b46M40; b47M40; f17GM40; f20GM40; f31GM40; f34GM40; f36GM40; f48GM40; f49GM40; mGM40; qGM40; sGM40; Violin; am040; am040.in; gm040
    { 0x113DD31,0x0066621, 0x16,0x00, 0x8,+0 }, // 40: GM41; HMIGM41; b45M41; f17GM41; f34GM41; mGM41; sGM41; Viola; am041; am041.in
    { 0x11CD171,0x00C6131, 0x49,0x00, 0x8,+0 }, // 41: GM42; HMIGM42; b45M42; f34GM42; sGM42; Cello; am042; am042.in
    { 0x1127121,0x0067223, 0x4D,0x80, 0x2,+0 }, // 42: GM43; HMIGM43; b45M43; f17GM43; f29GM56; f30GM56; f34GM43; f35GM43; mGM43; sGM43; Contrabass; Trumpet; am043; am043.in
    { 0x121F1F1,0x0166FE1, 0x40,0x00, 0x2,+0 }, // 43: GM44; HMIGM44; b45M44; f17GM44; f34GM44; f35GM44; mGM44; Tremulo Strings; am044; am044.in
    { 0x175F502,0x0358501, 0x1A,0x80, 0x0,+0 }, // 44: GM45; HMIGM45; b45M45; f17GM45; f29GM51; f30GM51; f34GM45; mGM45; Pizzicato String; SynthStrings 2; am045; am045.in
    { 0x175F502,0x0F4F301, 0x1D,0x80, 0x0,+0 }, // 45: GM46; HMIGM46; b45M46; f15GM57; f15GM58; f17GM46; f26GM57; f26GM58; f29GM57; f29GM58; f30GM57; f30GM58; f34GM46; mGM46; oGM57; oGM58; Orchestral Harp; Trombone; Tuba; am046; am046.in
    { 0x105F510,0x0C3F211, 0x41,0x00, 0x2,+0 }, // 46: GM47; HMIGM47; b45M47; b46M47; b47M47; f17GM47; f20GM47; f30GM112; f34GM47; f36GM47; hamM14; intM14; mGM47; qGM47; rickM14; BSynth4; BSynth4.; Timpany; Tinkle Bell; am047; am047.in; gm047
    { 0x125B121,0x00872A2, 0x9B,0x01, 0xE,+0 }, // 47: GM48; HMIGM48; b45M48; f34GM48; String Ensemble1; am048; am048.in
    { 0x1037FA1,0x1073F21, 0x98,0x00, 0x0,+0 }, // 48: GM49; HMIGM49; b45M49; f34GM49; String Ensemble2; am049; am049.in
    { 0x012C1A1,0x0054F61, 0x93,0x00, 0xA,+0 }, // 49: GM50; HMIGM50; b45M50; f34GM50; hamM20; intM20; rickM20; PMellow; PMellow.; Synth Strings 1; am050; am050.in
    { 0x022C121,0x0054F61, 0x18,0x00, 0xC,+0 }, // 50: GM51; HMIGM51; b45M51; b46M51; b47M51; f20GM51; f31GM51; f34GM51; f36GM51; f48GM51; f49GM51; qGM51; sGM51; SynthStrings 2; am051; am051.in; gm051
    { 0x015F431,0x0058A72, 0x5B,0x83, 0x0,+0 }, // 51: GM52; HMIGM52; b45M52; f34GM52; rickM85; Choir Aahs; Choir.in; am052; am052.in
    { 0x03974A1,0x0677161, 0x90,0x00, 0x0,+0 }, // 52: GM53; HMIGM53; b45M53; f34GM53; rickM86; sGM53; Oohs.ins; Voice Oohs; am053; am053.in
    { 0x0055471,0x0057A72, 0x57,0x00, 0xC,+0 }, // 53: GM54; HMIGM54; b45M54; f34GM54; sGM54; Synth Voice; am054; am054.in
    { 0x0635490,0x045A541, 0x00,0x00, 0x8,+0 }, // 54: GM55; HMIGM55; b45M55; f34GM55; Orchestra Hit; am055; am055.in
    { 0x0178521,0x0098F21, 0x92,0x01, 0xC,+0 }, // 55: GM56; HMIGM56; b45M56; b46M56; b47M56; f17GM56; f20GM56; f31GM56; f34GM56; f36GM56; f49GM56; mGM56; qGM56; Trumpet; am056; am056.in; gm056
    { 0x0177521,0x0098F21, 0x94,0x05, 0xC,+0 }, // 56: GM57; HMIGM57; b45M57; f17GM57; f29GM90; f30GM90; f34GM57; mGM57; Pad 3 polysynth; Trombone; am057; am057.in
    { 0x0157621,0x0378261, 0x94,0x00, 0xC,+0 }, // 57: GM58; HMIGM58; b45M58; f34GM58; Tuba; am058; am058.in
    { 0x1179E31,0x12C6221, 0x43,0x00, 0x2,+0 }, // 58: GM59; HMIGM59; b45M59; f17GM59; f34GM59; f35GM59; mGM59; sGM59; Muted Trumpet; am059; am059.in
    { 0x06A6121,0x00A7F21, 0x9B,0x00, 0x2,+0 }, // 59: GM60; HMIGM60; b45M60; f17GM60; f29GM92; f29GM93; f30GM92; f30GM93; f34GM60; f48GM62; mGM60; French Horn; Pad 5 bowedpad; Pad 6 metallic; Synth Brass 1; am060; am060.in
    { 0x01F7561,0x00F7422, 0x8A,0x06, 0x8,+0 }, // 60: GM61; HMIGM61; b45M61; f34GM61; Brass Section; am061; am061.in
    { 0x15572A1,0x0187121, 0x86,0x83, 0x0,+0 }, // 61: GM62; b45M62; f34GM62; sGM62; Synth Brass 1; am062
    { 0x03C5421,0x01CA621, 0x4D,0x00, 0x8,+0 }, // 62: GM63; HMIGM63; b45M63; f17GM63; f29GM26; f29GM44; f30GM26; f30GM44; f34GM63; mGM63; sGM63; Electric Guitar1; Synth Brass 2; Tremulo Strings; am063; am063.in
    { 0x1029331,0x00B7261, 0x8F,0x00, 0x8,+0 }, // 63: GM64; HMIGM64; b45M64; f34GM64; sGM64; Soprano Sax; am064; am064.in
    { 0x1039331,0x0097261, 0x8E,0x00, 0x8,+0 }, // 64: GM65; HMIGM65; b45M65; f34GM65; sGM65; Alto Sax; am065; am065.in
    { 0x1039331,0x0098261, 0x91,0x00, 0xA,+0 }, // 65: GM66; HMIGM66; b45M66; f34GM66; sGM66; Tenor Sax; am066; am066.in
    { 0x10F9331,0x00F7261, 0x8E,0x00, 0xA,+0 }, // 66: GM67; HMIGM67; b45M67; f34GM67; sGM67; Baritone Sax; am067; am067.in
    { 0x116AA21,0x00A8F21, 0x4B,0x00, 0x8,+0 }, // 67: GM68; HMIGM68; b45M68; f17GM68; f29GM84; f30GM84; f34GM68; mGM68; Lead 5 charang; Oboe; am068; am068.in
    { 0x1177E31,0x10C8B21, 0x90,0x00, 0x6,+0 }, // 68: GM69; HMIGM69; b45M69; f17GM69; f29GM85; f30GM85; f34GM69; f35GM69; mGM69; sGM69; English Horn; Lead 6 voice; am069; am069.in
    { 0x1197531,0x0196132, 0x81,0x00, 0x0,+0 }, // 69: GM70; HMIGM70; b45M70; f17GM70; f29GM86; f30GM86; f34GM70; mGM70; Bassoon; Lead 7 fifths; am070; am070.in
    { 0x0219B32,0x0177221, 0x90,0x00, 0x4,+0 }, // 70: GM71; HMIGM71; b45M71; f17GM71; f29GM82; f29GM83; f30GM82; f30GM83; f34GM71; mGM71; Clarinet; Lead 3 calliope; Lead 4 chiff; am071; am071.in
    { 0x05F85E1,0x01A65E1, 0x1F,0x00, 0x0,+0 }, // 71: GM72; HMIGM72; b45M72; f17GM72; f34GM72; f35GM72; mGM72; Piccolo; am072; am072.in
    { 0x05F88E1,0x01A65E1, 0x46,0x00, 0x0,+0 }, // 72: GM73; HMIGM73; b45M73; f17GM73; f29GM72; f29GM73; f30GM72; f30GM73; f34GM73; mGM73; Flute; Piccolo; am073; am073.in
    { 0x01F75A1,0x00A7521, 0x9C,0x00, 0x2,+0 }, // 73: GM74; HMIGM74; b45M74; sGM74; Recorder; am074; am074.in
    { 0x0588431,0x01A6521, 0x8B,0x00, 0x0,+0 }, // 74: GM75; HMIGM75; b45M75; f17GM75; f29GM77; f30GM77; f34GM75; f35GM75; mGM75; sGM75; Pan Flute; Shakuhachi; am075; am075.in
    { 0x05666E1,0x02665A1, 0x4C,0x00, 0x0,+0 }, // 75: GM76; HMIGM76; b45M76; f34GM76; sGM76; Bottle Blow; am076; am076.in
    { 0x0467662,0x03655A1, 0xCB,0x00, 0x0,+0 }, // 76: GM77; HMIGM77; b45M77; f34GM77; sGM77; Shakuhachi; am077; am077.in
    { 0x0075762,0x00756A1, 0x99,0x00, 0xB,+0 }, // 77: GM78; HMIGM78; b45M78; f34GM78; sGM78; Whistle; am078; am078.in
    { 0x0077762,0x00776A1, 0x93,0x00, 0xB,+0 }, // 78: GM79; HMIGM79; b45M79; f34GM79; hamM61; sGM79; Ocarina; am079; am079.in; ocarina
    { 0x203FF22,0x00FFF21, 0x59,0x00, 0x0,+0 }, // 79: GM80; HMIGM80; b45M80; f17GM80; f29GM47; f30GM47; f34GM80; f35GM80; f47GM80; hamM16; hamM65; intM16; mGM80; rickM16; sGM80; LSquare; LSquare.; Lead 1 squareea; Timpany; am080; am080.in; squarewv
    { 0x10FFF21,0x10FFF21, 0x0E,0x00, 0x0,+0 }, // 80: GM81; HMIGM81; b45M81; f17GM81; f34GM81; mGM81; sGM81; Lead 2 sawtooth; am081; am081.in
    { 0x0558622,0x0186421, 0x46,0x80, 0x0,+0 }, // 81: GM82; HMIGM82; b45M82; f17GM82; f34GM82; mGM82; sGM82; Lead 3 calliope; am082; am082.in
    { 0x0126621,0x00A96A1, 0x45,0x00, 0x0,+0 }, // 82: GM83; HMIGM83; b45M83; f34GM83; sGM83; Lead 4 chiff; am083; am083.in
    { 0x12A9221,0x02A9122, 0x8B,0x00, 0x0,+0 }, // 83: GM84; HMIGM84; b45M84; f17GM84; f34GM84; mGM84; sGM84; Lead 5 charang; am084; am084.in
    { 0x005DFA2,0x0076F61, 0x9E,0x40, 0x2,+0 }, // 84: GM85; HMIGM85; b45M85; f34GM85; hamM17; intM17; rickM17; rickM87; sGM85; Lead 6 voice; PFlutes; PFlutes.; Solovox.; am085; am085.in
    { 0x001EF20,0x2068F60, 0x1A,0x00, 0x0,+0 }, // 85: GM86; HMIGM86; b45M86; f34GM86; rickM93; sGM86; Lead 7 fifths; Saw_wave; am086; am086.in
    { 0x029F121,0x009F421, 0x8F,0x80, 0xA,+0 }, // 86: GM87; HMIGM87; b45M87; b46M87; b47M87; f17GM87; f20GM87; f31GM87; f34GM87; f35GM87; f36GM87; mGM87; qGM87; sGM87; Lead 8 brass; am087; am087.in; gm087
    { 0x0945377,0x005A0A1, 0xA5,0x00, 0x2,+0 }, // 87: GM88; HMIGM88; b45M88; f34GM88; sGM88; Pad 1 new age; am088; am088.in
    { 0x011A861,0x00325B1, 0x1F,0x80, 0xA,+0 }, // 88: GM89; HMIGM89; b45M89; b46M89; b47M89; f20GM89; f31GM89; f34GM89; f36GM89; f49GM89; qGM89; sGM89; Pad 2 warm; am089; am089.in; gm089
    { 0x0349161,0x0165561, 0x17,0x00, 0xC,+0 }, // 89: GM90; HMIGM90; b45M90; f34GM90; hamM21; intM21; rickM21; sGM90; LTriang; LTriang.; Pad 3 polysynth; am090; am090.in
    { 0x0015471,0x0036A72, 0x5D,0x00, 0x0,+0 }, // 90: GM91; HMIGM91; b45M91; f34GM91; rickM95; sGM91; Pad 4 choir; Spacevo.; am091; am091.in
    { 0x0432121,0x03542A2, 0x97,0x00, 0x8,+0 }, // 91: GM92; HMIGM92; b45M92; f34GM92; f47GM92; sGM92; Pad 5 bowedpad; am092; am092.in
    { 0x177A1A1,0x1473121, 0x1C,0x00, 0x0,+0 }, // 92: GM93; HMIGM93; b45M93; f34GM93; hamM22; intM22; rickM22; sGM93; PSlow; PSlow.in; Pad 6 metallic; am093; am093.in
    { 0x0331121,0x0254261, 0x89,0x03, 0xA,+0 }, // 93: GM94; HMIGM94; b45M94; f34GM94; hamM23; hamM54; intM23; rickM23; rickM96; sGM94; Halopad.; PSweep; PSweep.i; Pad 7 halo; am094; am094.in; halopad
    { 0x14711A1,0x007CF21, 0x15,0x00, 0x0,+0 }, // 94: GM95; HMIGM95; b45M95; f34GM95; f47GM95; hamM66; rickM97; Pad 8 sweep; Sweepad.; am095; am095.in; sweepad
    { 0x0F6F83A,0x0028651, 0xCE,0x00, 0x2,+0 }, // 95: GM96; HMIGM96; b45M96; f34GM96; sGM96; FX 1 rain; am096; am096.in
    { 0x1232121,0x0134121, 0x15,0x00, 0x0,+0 }, // 96: GM97; HMIGM97; b45M97; f17GM97; f29GM36; f30GM36; f34GM97; mGM97; sGM97; FX 2 soundtrack; Slap Bass 1; am097; am097.in
    { 0x0957406,0x072A501, 0x5B,0x00, 0x0,+0 }, // 97: GM98; HMIGM98; b45M98; f17GM98; f34GM98; f35GM98; mGM98; sGM98; FX 3 crystal; am098; am098.in
    { 0x081B122,0x026F261, 0x92,0x83, 0xC,+0 }, // 98: GM99; HMIGM99; b45M99; f34GM99; sGM99; FX 4 atmosphere; am099; am099.in
    { 0x151F141,0x0F5F242, 0x4D,0x00, 0x0,+0 }, // 99: GM100; HMIGM100; b45M100; f34GM100; hamM51; sGM100; FX 5 brightness; am100; am100.in
    { 0x1511161,0x01311A3, 0x94,0x80, 0x6,+0 }, // 100: GM101; HMIGM101; b45M101; f34GM101; sGM101; FX 6 goblins; am101; am101.in
    { 0x0311161,0x0031DA1, 0x8C,0x80, 0x6,+0 }, // 101: GM102; HMIGM102; b45M102; f34GM102; rickM98; sGM102; Echodrp1; FX 7 echoes; am102; am102.in
    { 0x173F3A4,0x0238161, 0x4C,0x00, 0x4,+0 }, // 102: GM103; HMIGM103; b45M103; f34GM103; sGM103; FX 8 sci-fi; am103; am103.in
    { 0x053D202,0x1F6F207, 0x85,0x03, 0x0,+0 }, // 103: GM104; HMIGM104; b45M104; f17GM104; f29GM63; f30GM63; f34GM104; mGM104; sGM104; Sitar; Synth Brass 2; am104; am104.in
    { 0x111A311,0x0E5A213, 0x0C,0x80, 0x0,+0 }, // 104: GM105; HMIGM105; b45M105; f17GM105; f34GM105; mGM105; sGM105; Banjo; am105; am105.in
    { 0x141F611,0x2E6F211, 0x06,0x00, 0x4,+0 }, // 105: GM106; HMIGM106; b45M106; f17GM106; f34GM106; hamM24; intM24; mGM106; rickM24; sGM106; LDist; LDist.in; Shamisen; am106; am106.in
    { 0x032D493,0x111EB91, 0x91,0x00, 0x8,+0 }, // 106: GM107; HMIGM107; b45M107; f34GM107; sGM107; Koto; am107; am107.in
    { 0x056FA04,0x005C201, 0x4F,0x00, 0xC,+0 }, // 107: GM108; HMIGM108; b45M108; b46M108; b47M108; f17GM108; f20GM108; f31GM108; f31GM45; f34GM108; f35GM108; f36GM108; f48GM108; f49GM108; hamM57; mGM108; qGM108; sGM108; Kalimba; Pizzicato String; am108; am108.in; gm108; kalimba
    { 0x0207C21,0x10C6F22, 0x49,0x00, 0x6,+0 }, // 108: GM109; HMIGM109; b45M109; b46M109; b47M109; f17GM109; f20GM109; f31GM109; f34GM109; f35GM109; f36GM109; f48GM109; f49GM109; mGM109; qGM109; sGM109; Bagpipe; am109; am109.in; gm109
    { 0x133DD31,0x0165621, 0x85,0x00, 0xA,+0 }, // 109: GM110; HMIGM110; b45M110; b46M110; b47M110; f17GM110; f20GM110; f31GM110; f34GM110; f35GM110; f36GM110; f48GM110; f49GM110; mGM110; qGM110; sGM110; Fiddle; am110; am110.in; gm110
    { 0x205DA20,0x00B8F21, 0x04,0x81, 0x6,+0 }, // 110: GM111; HMIGM111; b45M111; f17GM111; f34GM111; f35GM111; mGM111; sGM111; Shanai; am111; am111.in
    { 0x0E5F105,0x0E5C303, 0x6A,0x80, 0x6,+0 }, // 111: GM112; HMIGM112; b45M112; b46M112; b47M112; f17GM112; f20GM112; f31GM112; f34GM112; f36GM112; f48GM112; mGM112; qGM112; sGM112; Tinkle Bell; am112; am112.in; gm112
    { 0x026EC07,0x016F802, 0x15,0x00, 0xA,+0 }, // 112: GM113; HMIGM113; b45M113; f17GM113; f34GM113; f35GM113; hamM50; mGM113; sGM113; Agogo Bells; agogo; am113; am113.in
    { 0x0356705,0x005DF01, 0x9D,0x00, 0x8,+0 }, // 113: GM114; HMIGM114; b45M114; f17GM114; f34GM114; f35GM114; mGM114; sGM114; Steel Drums; am114; am114.in
    { 0x028FA18,0x0E5F812, 0x96,0x00, 0xA,+0 }, // 114: GM115; HMIGM115; b45M115; f17GM115; f34GM115; f35GM115; mGM115; rickM100; sGM115; Woodblk.; Woodblock; am115; am115.in
    { 0x007A810,0x003FA00, 0x86,0x03, 0x6,+0 }, // 115: GM116; HMIGM116; b45M116; b46M116; b47M116; f17GM116; f20GM116; f29GM118; f30GM117; f30GM118; f31GM116; f34GM116; f35GM116; f36GM116; f49GM116; hamM69; hamP90; mGM116; qGM116; Melodic Tom; Synth Drum; Taiko; Taiko Drum; am116; am116.in; gm116; taiko
    { 0x247F811,0x003F310, 0x41,0x03, 0x4,+0 }, // 116: GM117; HMIGM117; b45M117; f17GM117; f29GM113; f30GM113; f34GM117; f35GM117; hamM58; mGM117; sGM117; Agogo Bells; Melodic Tom; am117; am117.in; melotom
    { 0x206F101,0x002F310, 0x8E,0x00, 0xE,+0 }, // 117: GM118; HMIGM118; b45M118; f17GM118; f34GM118; mGM118; Synth Drum; am118; am118.in
    { 0x0001F0E,0x3FF1FC0, 0x00,0x00, 0xE,+0 }, // 118: GM119; HMIGM119; b45M119; f34GM119; mGM119; Reverse Cymbal; am119; am119.in
    { 0x024F806,0x2845603, 0x80,0x88, 0xE,+0 }, // 119: GM120; HMIGM120; b45M120; f17GM120; f34GM120; f35GM120; hamM36; intM36; mGM120; rickM101; rickM36; sGM120; DNoise1; DNoise1.; Fretnos.; Guitar FretNoise; am120; am120.in
    { 0x000F80E,0x30434D0, 0x00,0x05, 0xE,+0 }, // 120: GM121; HMIGM121; b45M121; f17GM121; f34GM121; f35GM121; mGM121; sGM121; Breath Noise; am121; am121.in
    { 0x000F60E,0x3021FC0, 0x00,0x00, 0xE,+0 }, // 121: GM122; HMIGM122; b45M122; f17GM122; f34GM122; mGM122; sGM122; Seashore; am122; am122.in
    { 0x0A337D5,0x03756DA, 0x95,0x40, 0x0,+0 }, // 122: GM123; HMIGM123; b45M123; f15GM124; f17GM123; f26GM124; f29GM124; f30GM124; f34GM123; mGM123; sGM123; Bird Tweet; Telephone; am123; am123.in
    { 0x261B235,0x015F414, 0x5C,0x08, 0xA,+0 }, // 123: GM124; HMIGM124; b45M124; f17GM124; f29GM123; f30GM123; f34GM124; mGM124; sGM124; Bird Tweet; Telephone; am124; am124.in
    { 0x000F60E,0x3F54FD0, 0x00,0x00, 0xE,+0 }, // 124: GM125; HMIGM125; b45M125; f17GM125; f34GM125; mGM125; sGM125; Helicopter; am125; am125.in
    { 0x001FF26,0x11612E4, 0x00,0x00, 0xE,+0 }, // 125: GM126; HMIGM126; b45M126; f17GM126; f34GM126; f35GM126; mGM126; sGM126; Applause/Noise; am126; am126.in
    { 0x0F0F300,0x2C9F600, 0x00,0x00, 0xE,+0 }, // 126: GM127; HMIGM127; b45M127; f17GM127; f34GM127; mGM127; sGM127; Gunshot; am127; am127.in
    { 0x277F810,0x006F311, 0x44,0x00, 0x8,+0 }, // 127: GP35; GP36; b45P0; b45P1; b45P10; b45P11; b45P12; b45P13; b45P14; b45P15; b45P16; b45P17; b45P18; b45P19; b45P2; b45P20; b45P21; b45P22; b45P23; b45P24; b45P25; b45P26; b45P27; b45P28; b45P29; b45P3; b45P30; b45P31; b45P32; b45P33; b45P34; b45P35; b45P36; b45P4; b45P5; b45P6; b45P7; b45P8; b45P9; b46P35; f17GP35; f17GP36; f20GP35; f20GP36; f29GP35; f29GP36; f30GP35; f30GP36; f31GP31; f31GP35; f31GP36; f34GP35; f34GP36; f35GP35; f42GP36; hamP11; hamP34; hamP35; intP34; intP35; mGP35; mGP36; qGP35; qGP36; rickP14; rickP34; rickP35; Ac Bass Drum; Bass Drum 1; apo035; apo035.i; aps035; gps035; kick2.in
    { 0x0FFF902,0x0FFF811, 0x07,0x00, 0x8,+0 }, // 128: GP37; b45P37; f17GP37; f23GP54; f29GP37; f30GP37; f34GP37; f49GP37; mGP37; Side Stick; Tambourine; aps037
    { 0x205FC00,0x017FA00, 0x00,0x00, 0xE,+0 }, // 129: GP38; GP40; b45P38; b45P40; b46P38; b46P40; f17GP38; f17GP40; f20GP38; f20GP40; f29GP38; f29GP40; f30GP38; f30GP40; f31GP38; f34GP38; f34GP40; f49GP38; mGP38; mGP40; qGP38; qGP40; Acoustic Snare; Electric Snare; aps038; aps040; gps038; gps040
    { 0x007FF00,0x008FF01, 0x02,0x00, 0x0,+0 }, // 130: GP39; b45P39; f17GP39; f29GP39; f30GP39; f34GP39; f49GP39; mGP39; Hand Clap; aps039
    { 0x00CF600,0x006F600, 0x00,0x00, 0x4,+0 }, // 131: GP41; GP43; GP45; GP47; GP48; GP50; GP87; b45P100; b45P101; b45P102; b45P103; b45P104; b45P105; b45P106; b45P107; b45P108; b45P109; b45P110; b45P111; b45P112; b45P113; b45P114; b45P115; b45P116; b45P117; b45P118; b45P119; b45P120; b45P121; b45P122; b45P123; b45P124; b45P125; b45P126; b45P127; b45P41; b45P43; b45P45; b45P47; b45P48; b45P50; b45P87; b45P88; b45P89; b45P90; b45P91; b45P92; b45P93; b45P94; b45P95; b45P96; b45P97; b45P98; b45P99; f17GP41; f17GP43; f17GP45; f17GP47; f17GP48; f17GP50; f17GP87; f29GP41; f29GP43; f29GP45; f29GP47; f29GP48; f29GP50; f29GP87; f30GP41; f30GP43; f30GP45; f30GP47; f30GP48; f30GP50; f30GP87; f34GP41; f34GP43; f34GP45; f34GP47; f34GP48; f34GP50; f34GP87; f35GP41; f35GP43; f35GP45; f35GP47; f35GP48; f35GP50; f35GP87; f42GP41; f42GP43; f42GP45; f42GP47; f42GP48; f42GP50; f49GP41; f49GP43; f49GP45; f49GP47; f49GP48; f49GP50; f49GP87; hamP1; hamP2; hamP3; hamP4; hamP5; hamP6; mGP41; mGP43; mGP45; mGP47; mGP48; mGP50; mGP87; rickP105; sGP87; High Floor Tom; High Tom; High-Mid Tom; Low Floor Tom; Low Tom; Low-Mid Tom; Open Surdu; aps041; aps087; surdu.in
    { 0x008F60C,0x247FB12, 0x00,0x00, 0xA,+0 }, // 132: GP42; b45P42; b46P42; f17GP42; f20GP42; f23GP68; f23GP70; f29GP42; f30GP42; f31GP42; f34GP1; f34GP42; hamP55; intP55; mGP42; qGP42; rickP55; Closed High Hat; Low Agogo; Maracas; aps042; aps042.i; gps042
    { 0x008F60C,0x2477B12, 0x00,0x05, 0xA,+0 }, // 133: GP44; b45P44; f17GP44; f29GP44; f30GP44; f34GP44; f35GP44; f49GP44; mGP44; Pedal High Hat; aps044
    { 0x002F60C,0x243CB12, 0x00,0x00, 0xA,+0 }, // 134: GP46; b45P46; b46P46; f17GP46; f20GP46; f29GP46; f30GP46; f31GP46; f34GP46; f49GP46; mGP46; qGP46; Open High Hat; aps046; gps046
    { 0x000F60E,0x3029FD0, 0x00,0x00, 0xE,+0 }, // 135: GP49; GP57; b45P49; b45P57; f15GP49; f17GP49; f17GP57; f26GP49; f29GP49; f29GP57; f30GP49; f30GP57; f34GP49; f34GP57; f35GP49; f49GP49; f49GP57; hamP0; mGP49; mGP57; oGP49; Crash Cymbal 1; Crash Cymbal 2; aps049; aps057; crash1
    { 0x042F80E,0x3E4F407, 0x08,0x4A, 0xE,+0 }, // 136: GP51; GP59; b45P51; b45P59; f17GP51; f17GP59; f29GM119; f29GM125; f29GM127; f29GP51; f29GP59; f30GM119; f30GM125; f30GM127; f30GP51; f30GP59; f34GP51; f34GP59; f35GP51; f35GP59; f49GP51; f49GP59; mGP51; mGP59; sGP51; sGP59; Gunshot; Helicopter; Reverse Cymbal; Ride Cymbal 1; Ride Cymbal 2; aps051
    { 0x030F50E,0x0029FD0, 0x00,0x0A, 0xE,+0 }, // 137: GP52; b45P52; f17GP52; f29GP52; f30GP52; f34GP52; f35GP52; f49GP52; hamP19; mGP52; Chinese Cymbal; aps052
    { 0x3E4E40E,0x1E5F507, 0x0A,0x5D, 0x6,+0 }, // 138: GP53; b45P53; f17GP53; f29GP53; f30GP53; f34GP53; f35GP53; f49GP53; mGP53; sGP53; Ride Bell; aps053
    { 0x004B402,0x0F79705, 0x03,0x0A, 0xE,+0 }, // 139: GP54; b45P54; f17GP54; f30GP54; f34GP54; f49GP54; mGP54; Tambourine; aps054
    { 0x000F64E,0x3029F9E, 0x00,0x00, 0xE,+0 }, // 140: GP55; b45P55; f34GP55; Splash Cymbal; aps055
    { 0x237F811,0x005F310, 0x45,0x08, 0x8,+0 }, // 141: GP56; b45P56; f17GP56; f29GP56; f30GP56; f34GP56; f48GP56; f49GP56; mGP56; sGP56; Cow Bell; aps056
    { 0x303FF80,0x014FF10, 0x00,0x0D, 0xC,+0 }, // 142: GP58; b45P58; f34GP58; Vibraslap; aps058
    { 0x00CF506,0x008F502, 0x0B,0x00, 0x6,+0 }, // 143: GP60; b45P60; f17GP60; f29GP60; f30GP60; f34GP60; f48GP60; f49GP60; mGP60; sGP60; High Bongo; aps060
    { 0x0BFFA01,0x097C802, 0x00,0x00, 0x7,+0 }, // 144: GP61; b45P61; f15GP61; f17GP61; f26GP61; f29GP61; f30GP61; f34GP61; f48GP61; f49GP61; mGP61; oGP61; sGP61; Low Bongo; aps061
    { 0x087FA01,0x0B7FA01, 0x51,0x00, 0x6,+0 }, // 145: GP62; b45P62; b46P62; f17GP62; f20GP62; f29GP62; f30GP62; f31GP62; f34GP62; f48GP62; f49GP62; mGP62; qGP62; sGP62; Mute High Conga; aps062; gps062
    { 0x08DFA01,0x0B8F802, 0x54,0x00, 0x6,+0 }, // 146: GP63; b45P63; f17GP63; f29GP63; f30GP63; f34GP63; f48GP63; f49GP63; mGP63; sGP63; Open High Conga; aps063
    { 0x088FA01,0x0B6F802, 0x59,0x00, 0x6,+0 }, // 147: GP64; b45P64; f17GP64; f29GP64; f30GP64; f34GP64; f48GP64; f49GP64; mGP64; sGP64; Low Conga; aps064
    { 0x30AF901,0x006FA00, 0x00,0x00, 0xE,+0 }, // 148: GP65; b45P65; f17GP65; f29GP65; f30GP65; f34GP65; f35GP65; f35GP66; f48GP65; f49GP65; hamP8; mGP65; rickP98; rickP99; sGP65; High Timbale; Low Timbale; aps065; timbale; timbale.
    { 0x389F900,0x06CF600, 0x80,0x00, 0xE,+0 }, // 149: GP66; b45P66; b46P66; f17GP66; f20GP66; f30GP66; f31GP66; f34GP66; f48GP66; f49GP66; mGP66; qGP66; sGP66; Low Timbale; aps066; gps066
    { 0x388F803,0x0B6F60C, 0x80,0x08, 0xF,+0 }, // 150: GP67; b45P67; f17GP67; f29GP67; f30GP67; f34GP67; f35GP67; f35GP68; f49GP67; mGP67; sGP67; High Agogo; Low Agogo; aps067
    { 0x388F803,0x0B6F60C, 0x85,0x00, 0xF,+0 }, // 151: GP68; b45P68; f17GP68; f29GP68; f30GP68; f34GP68; f49GP68; mGP68; sGP68; Low Agogo; aps068
    { 0x04F760E,0x2187700, 0x40,0x08, 0xE,+0 }, // 152: GP69; b45P69; f15GP69; f17GP69; f26GP69; f29GP69; f30GP69; f34GP69; f42GP69; f49GP69; mGP69; Cabasa; aps069
    { 0x049C80E,0x2699B03, 0x40,0x00, 0xE,+0 }, // 153: GP70; b45P70; f15GP70; f17GP70; f26GP70; f29GP70; f30GP70; f34GP70; f35GP70; f49GP70; mGP70; sGP70; Maracas; aps070
    { 0x305ADD7,0x0058DC7, 0xDC,0x00, 0xE,+0 }, // 154: GP71; b45P71; f15GP71; f17GP71; f26GP71; f29GP71; f30GP71; f34GP71; f35GP71; f48GP71; f49GP71; mGP71; sGP71; Short Whistle; aps071
    { 0x304A8D7,0x00488C7, 0xDC,0x00, 0xE,+0 }, // 155: GP72; b45P72; f15GP72; f17GP72; f26GP72; f29GP72; f30GP72; f34GP72; f35GP72; f48GP72; f49GP72; mGP72; sGP72; Long Whistle; aps072
    { 0x306F680,0x3176711, 0x00,0x00, 0xE,+0 }, // 156: GP73; b45P73; f34GP73; rickP96; sGP73; Short Guiro; aps073; guiros.i
    { 0x205F580,0x3164611, 0x00,0x09, 0xE,+0 }, // 157: GP74; b45P74; f34GP74; sGP74; Long Guiro; aps074
    { 0x0F40006,0x0F5F715, 0x3F,0x00, 0x1,+0 }, // 158: GP75; b45P75; f15GP75; f17GP75; f26GP75; f29GP75; f30GP75; f34GP75; f49GP75; mGP75; oGP75; sGP75; Claves; aps075
    { 0x3F40006,0x0F5F712, 0x3F,0x00, 0x0,+0 }, // 159: GP76; b45P76; b46P76; b46P77; f17GP76; f20GP76; f20GP77; f29GP76; f30GP76; f31GP76; f31GP77; f34GP76; f35GP76; f48GP76; f49GP76; mGP76; qGP76; qGP77; High Wood Block; Low Wood Block; aps076; gps076; gps077
    { 0x0F40006,0x0F5F712, 0x3F,0x00, 0x1,+0 }, // 160: GP77; b45P77; f17GP77; f29GP77; f30GP77; f34GP77; f35GP77; f48GP77; f49GP77; mGP77; sGP77; Low Wood Block; aps077
    { 0x0E76701,0x0077502, 0x58,0x00, 0x0,+0 }, // 161: GP78; b45P78; f17GP78; f29GP78; f30GP78; f34GP78; f35GP78; f49GP78; mGP78; sGP78; Mute Cuica; aps078
    { 0x048F841,0x0057542, 0x45,0x08, 0x0,+0 }, // 162: GP79; b45P79; f34GP79; sGP79; Open Cuica; aps079
    { 0x3F0E00A,0x005FF1E, 0x40,0x4E, 0x8,+0 }, // 163: GP80; b45P80; f17GP80; f29GP80; f30GP80; f34GP80; f35GP80; f49GP80; mGP80; sGP80; Mute Triangle; aps080
    { 0x3F0E00A,0x002FF1E, 0x7C,0x52, 0x8,+0 }, // 164: GP81; b45P81; f17GP81; f29GM121; f29GP81; f30GM121; f30GP81; f34GP81; f35GP81; f49GP81; mGP81; sGP81; Breath Noise; Open Triangle; aps081
    { 0x04A7A0E,0x21B7B00, 0x40,0x08, 0xE,+0 }, // 165: GP82; b45P82; f17GP82; f29GP82; f30GP82; f34GP82; f35GP82; f42GP82; f49GP82; hamP7; mGP82; sGP82; Shaker; aps082
    { 0x3E4E40E,0x1395507, 0x0A,0x40, 0x6,+0 }, // 166: GP83; b45P83; f17GP83; f29GP83; f30GP83; f34GP83; f35GP83; f48GP83; f49GP83; mGP83; sGP83; Jingle Bell; aps083
    { 0x332F905,0x0A5D604, 0x05,0x40, 0xE,+0 }, // 167: GP84; b45P84; f15GP51; f17GP84; f26GP51; f29GM126; f29GP84; f30GM126; f30GP84; f34GP84; f35GP84; f49GP84; mGP84; sGP84; Applause/Noise; Bell Tree; Ride Cymbal 1; aps084
    { 0x3F30002,0x0F5F715, 0x3F,0x00, 0x8,+0 }, // 168: GP85; b45P85; f17GP85; f29GM120; f29GP85; f30GM120; f30GP85; f34GP85; f35GP85; f48GP85; f49GP85; mGP85; sGP85; Castanets; Guitar FretNoise; aps085
    { 0x08DFA01,0x0B5F802, 0x4F,0x00, 0x7,+0 }, // 169: GP86; b45P86; f15GP63; f15GP64; f17GP86; f26GP63; f26GP64; f29GP86; f30GP86; f34GP86; f35GP86; f49GP86; mGP86; sGP86; Low Conga; Mute Surdu; Open High Conga; aps086
    { 0x1199523,0x0198421, 0x48,0x00, 0x8,+0 }, // 170: HMIGM0; HMIGM29; f17GM29; f35GM29; mGM29; Overdrive Guitar; am029.in
    { 0x054F231,0x056F221, 0x4B,0x00, 0x8,+0 }, // 171: HMIGM1; f17GM1; mGM1; BrightAcouGrand; am001.in
    { 0x055F231,0x076F221, 0x49,0x00, 0x8,+0 }, // 172: HMIGM2; f17GM2; f35GM2; mGM2; ElecGrandPiano; am002.in
    { 0x03BF2B1,0x00BF361, 0x0E,0x00, 0x6,+0 }, // 173: HMIGM3; am003.in
    { 0x038F101,0x028F121, 0x57,0x00, 0x0,+0 }, // 174: HMIGM4; f17GM4; f35GM4; mGM4; Rhodes Piano; am004.in
    { 0x038F101,0x028F121, 0x93,0x00, 0x0,+0 }, // 175: HMIGM5; f17GM5; f35GM5; mGM5; Chorused Piano; am005.in
    { 0x001A221,0x0D5F136, 0x80,0x0E, 0x8,+0 }, // 176: HMIGM6; f17GM6; mGM6; Harpsichord; am006.in
    { 0x0A8C201,0x058C201, 0x92,0x00, 0xA,+0 }, // 177: HMIGM7; f17GM7; mGM7; Clavinet; am007.in
    { 0x054F60C,0x0B5F381, 0x5C,0x00, 0x0,+0 }, // 178: HMIGM8; am008.in
    { 0x032F607,0x011F511, 0x97,0x80, 0x2,+0 }, // 179: HMIGM9; f17GM9; mGM9; Glockenspiel; am009.in
    { 0x0045617,0x004F601, 0x21,0x00, 0x2,+0 }, // 180: HMIGM10; b46M10; b47M10; f17GM10; f20GM10; f31GM10; f35GM10; f36GM10; f48GM10; f49GM10; mGM10; qGM10; Music box; am010.in; gm010
    { 0x0E6F318,0x0F6F281, 0x62,0x00, 0x0,+0 }, // 181: HMIGM11; am011.in
    { 0x055F718,0x0D8E521, 0x23,0x00, 0x0,+0 }, // 182: HMIGM12; f17GM12; mGM12; Marimba; am012.in
    { 0x0A6F615,0x0E6F601, 0x91,0x00, 0x4,+0 }, // 183: HMIGM13; f17GM13; f35GM13; mGM13; Xylophone; am013.in
    { 0x082D345,0x0E3A381, 0x59,0x80, 0xC,+0 }, // 184: HMIGM14; am014.in
    { 0x1557403,0x005B381, 0x49,0x80, 0x4,+0 }, // 185: HMIGM15; f48GM15; Dulcimer; am015.in
    { 0x122F603,0x0F3F321, 0x87,0x80, 0x6,+0 }, // 186: HMIGM27; f17GM27; mGM27; Electric Guitar2; am027.in
    { 0x09AA101,0x0DFF221, 0x89,0x40, 0x8,+0 }, // 187: HMIGM37; f17GM37; mGM37; Slap Bass 2; am037.in
    { 0x15572A1,0x0187121, 0x86,0x0D, 0x0,+0 }, // 188: HMIGM62; am062.in
    { 0x0F00010,0x0F00010, 0x3F,0x3F, 0x0,+0 }, // 189: HMIGP0; HMIGP1; HMIGP10; HMIGP100; HMIGP101; HMIGP102; HMIGP103; HMIGP104; HMIGP105; HMIGP106; HMIGP107; HMIGP108; HMIGP109; HMIGP11; HMIGP110; HMIGP111; HMIGP112; HMIGP113; HMIGP114; HMIGP115; HMIGP116; HMIGP117; HMIGP118; HMIGP119; HMIGP12; HMIGP120; HMIGP121; HMIGP122; HMIGP123; HMIGP124; HMIGP125; HMIGP126; HMIGP127; HMIGP13; HMIGP14; HMIGP15; HMIGP16; HMIGP17; HMIGP18; HMIGP19; HMIGP2; HMIGP20; HMIGP21; HMIGP22; HMIGP23; HMIGP24; HMIGP25; HMIGP26; HMIGP3; HMIGP4; HMIGP5; HMIGP6; HMIGP7; HMIGP8; HMIGP88; HMIGP89; HMIGP9; HMIGP90; HMIGP91; HMIGP92; HMIGP93; HMIGP94; HMIGP95; HMIGP96; HMIGP97; HMIGP98; HMIGP99; b42P0; b42P1; b42P10; b42P100; b42P101; b42P102; b42P103; b42P104; b42P105; b42P106; b42P107; b42P108; b42P109; b42P11; b42P110; b42P111; b42P112; b42P113; b42P114; b42P115; b42P116; b42P117; b42P118; b42P119; b42P12; b42P120; b42P121; b42P122; b42P123; b42P124; b42P125; b42P126; b42P13; b42P14; b42P15; b42P16; b42P17; b42P18; b42P19; b42P2; b42P20; b42P21; b42P22; b42P23; b42P24; b42P25; b42P26; b42P27; b42P3; b42P4; b42P5; b42P6; b42P7; b42P8; b42P88; b42P89; b42P9; b42P90; b42P91; b42P92; b42P93; b42P94; b42P95; b42P96; b42P97; b42P98; b42P99; b43P0; b43P1; b43P10; b43P100; b43P101; b43P102; b43P103; b43P104; b43P105; b43P106; b43P107; b43P108; b43P109; b43P11; b43P110; b43P111; b43P112; b43P113; b43P114; b43P115; b43P116; b43P117; b43P118; b43P119; b43P12; b43P120; b43P121; b43P122; b43P123; b43P124; b43P125; b43P126; b43P127; b43P13; b43P14; b43P15; b43P16; b43P17; b43P18; b43P19; b43P2; b43P20; b43P21; b43P22; b43P23; b43P24; b43P25; b43P26; b43P27; b43P3; b43P4; b43P5; b43P6; b43P7; b43P8; b43P88; b43P89; b43P9; b43P90; b43P91; b43P92; b43P93; b43P94; b43P95; b43P96; b43P97; b43P98; b43P99; b44M0; b44M1; b44M10; b44M100; b44M101; b44M102; b44M103; b44M104; b44M105; b44M106; b44M107; b44M108; b44M109; b44M11; b44M110; b44M111; b44M112; b44M113; b44M114; b44M115; b44M116; b44M117; b44M118; b44M119; b44M12; b44M120; b44M121; b44M122; b44M123; b44M124; b44M125; b44M126; b44M127; b44M13; b44M14; b44M15; b44M16; b44M17; b44M18; b44M19; b44M2; b44M20; b44M21; b44M22; b44M23; b44M24; b44M25; b44M26; b44M3; b44M4; b44M5; b44M6; b44M7; b44M8; b44M88; b44M89; b44M9; b44M90; b44M91; b44M92; b44M93; b44M94; b44M95; b44M96; b44M97; b44M98; b44M99; Blank; Blank.in
    { 0x0F1F02E,0x3487407, 0x00,0x07, 0x8,+0 }, // 190: HMIGP27; HMIGP28; HMIGP29; HMIGP30; HMIGP31; b44M27; b44M28; b44M29; b44M30; b44M31; Wierd1.i
    { 0x0FE5229,0x3D9850E, 0x00,0x07, 0x6,+0 }, // 191: HMIGP32; HMIGP33; HMIGP34; b44M32; b44M33; b44M34; b44P127; WIERD2.I; Wierd2.i
    { 0x0FE6227,0x3D9950A, 0x00,0x07, 0x8,+0 }, // 192: HMIGP35; b44M35; Wierd3.i
    { 0x0FDF800,0x0C7F601, 0x0B,0x00, 0x8,+0 }, // 193: HMIGP36; b43P36; Kick; Kick.ins
    { 0x0FBF116,0x069F911, 0x08,0x02, 0x0,+0 }, // 194: HMIGP37; HMIGP85; HMIGP86; b43P31; b43P37; b43P85; b43P86; b44M37; b44M85; b44M86; RimShot; RimShot.; rimShot; rimShot.; rimshot; rimshot.
    { 0x000FF26,0x0A7F802, 0x00,0x02, 0xE,+0 }, // 195: HMIGP38; HMIGP40; b43P38; b43P40; b44M38; b44M40; Snare; Snare.in
    { 0x00F9F30,0x0FAE83A, 0x00,0x00, 0xE,+0 }, // 196: HMIGP39; b43P28; b43P39; b44M39; Clap; Clap.ins; clap
    { 0x01FFA06,0x0F5F511, 0x0A,0x00, 0xF,+0 }, // 197: HMIGP41; HMIGP43; HMIGP45; HMIGP47; HMIGP48; HMIGP50; b43P41; b43P43; b43P45; b43P47; b43P48; b43P50; b44M41; b44M43; b44M45; b44M47; b44M48; b44M50; Toms; Toms.ins
    { 0x0F1F52E,0x3F99906, 0x05,0x02, 0x0,+0 }, // 198: HMIGP42; HMIGP44; b44M42; b44M44; clshat97
    { 0x0F89227,0x3D8750A, 0x00,0x03, 0x8,+0 }, // 199: HMIGP46; b44M46; Opnhat96
    { 0x2009F2C,0x3A4C50E, 0x00,0x09, 0xE,+0 }, // 200: HMIGP49; HMIGP52; HMIGP55; HMIGP57; b43P49; b43P52; b43P55; b43P57; b44M49; b44M52; b44M55; b44M57; Crashcym
    { 0x0009429,0x344F904, 0x10,0x0C, 0xE,+0 }, // 201: HMIGP51; HMIGP53; HMIGP59; b43P51; b43P53; b43P59; b44M51; b44M53; b44M59; Ridecym; Ridecym.; ridecym; ridecym.
    { 0x0F1F52E,0x3F78706, 0x09,0x02, 0x0,+0 }, // 202: HMIGP54; b43P54; b44M54; Tamb; Tamb.ins
    { 0x2F1F535,0x028F703, 0x19,0x02, 0x0,+0 }, // 203: HMIGP56; b43P56; b44M56; Cowbell; Cowbell.
    { 0x100FF80,0x1F7F500, 0x00,0x00, 0xC,+0 }, // 204: HMIGP58; b42P58; b43P58; b44M58; vibrasla
    { 0x0FAFA25,0x0F99803, 0xCD,0x00, 0x0,+0 }, // 205: HMIGP60; HMIGP62; b43P60; b43P62; b44M60; b44M62; mutecong
    { 0x1FAF825,0x0F7A803, 0x1B,0x00, 0x0,+0 }, // 206: HMIGP61; b43P61; b44M61; conga; conga.in
    { 0x1FAF825,0x0F69603, 0x21,0x00, 0xE,+0 }, // 207: HMIGP63; HMIGP64; b43P63; b43P64; b44M63; b44M64; loconga; loconga.
    { 0x2F5F504,0x236F603, 0x16,0x03, 0xA,+0 }, // 208: HMIGP65; HMIGP66; b43P65; b43P66; b44M65; b44M66; timbale; timbale.
    { 0x091F015,0x0E8A617, 0x1E,0x04, 0xE,+0 }, // 209: HMIGP67; HMIGP68; b43M113; b43P67; b43P68; AGOGO; agogo; agogo.in
    { 0x001FF0E,0x077780E, 0x06,0x04, 0xE,+0 }, // 210: HMIGP69; HMIGP70; HMIGP82; b43P69; b43P70; b43P82; b44M69; b44M70; b44M82; shaker; shaker.i
    { 0x2079F20,0x22B950E, 0x1C,0x00, 0x0,+0 }, // 211: HMIGP71; b43P71; b44M71; hiwhist; hiwhist.
    { 0x2079F20,0x23B940E, 0x1E,0x00, 0x0,+0 }, // 212: HMIGP72; b43P72; b44M72; lowhist; lowhist.
    { 0x0F7F020,0x33B8809, 0x00,0x00, 0xC,+0 }, // 213: HMIGP73; b43P73; b44M73; higuiro; higuiro.
    { 0x0F7F420,0x33B560A, 0x03,0x00, 0x0,+0 }, // 214: HMIGP74; b43P74; b44M74; loguiro; loguiro.
    { 0x05BF714,0x089F712, 0x4B,0x00, 0x0,+0 }, // 215: HMIGP75; b43P75; b44M75; clavecb; clavecb.
    { 0x0F2FA27,0x09AF612, 0x22,0x00, 0x0,+0 }, // 216: HMIGP76; HMIGP77; b43M115; b43P33; b43P76; b43P77; b44M76; b44M77; b44P115; WOODBLOK; woodblok
    { 0x1F75020,0x03B7708, 0x09,0x05, 0x0,+0 }, // 217: HMIGP78; b43P78; b44M78; hicuica; hicuica.
    { 0x1077F26,0x06B7703, 0x29,0x05, 0x0,+0 }, // 218: HMIGP79; b43P79; b44M79; locuica; locuica.
    { 0x0F0F126,0x0FCF727, 0x44,0x40, 0x6,+0 }, // 219: HMIGP80; b43P80; b44M80; mutringl
    { 0x0F0F126,0x0F5F527, 0x44,0x40, 0x6,+0 }, // 220: HMIGP81; HMIGP83; HMIGP84; b42P81; b42P83; b42P84; b43P81; b43P83; b43P84; b44M81; b44M83; b44M84; triangle
    { 0x0F3F821,0x0ADC620, 0x1C,0x00, 0xC,+0 }, // 221: HMIGP87; b43M116; b43P87; b44M87; b44P116; TAIKO; TAIKO.IN; taiko; taiko.in
    { 0x0FFFF01,0x0FFFF01, 0x3F,0x3F, 0x0,+0 }, // 222: hamM0; hamM100; hamM101; hamM102; hamM103; hamM104; hamM105; hamM106; hamM107; hamM108; hamM109; hamM110; hamM111; hamM112; hamM113; hamM114; hamM115; hamM116; hamM117; hamM118; hamM119; hamM126; hamM49; hamM74; hamM75; hamM76; hamM77; hamM78; hamM79; hamM80; hamM81; hamM82; hamM83; hamM84; hamM85; hamM86; hamM87; hamM88; hamM89; hamM90; hamM91; hamM92; hamM93; hamM94; hamM95; hamM96; hamM97; hamM98; hamM99; hamP100; hamP101; hamP102; hamP103; hamP104; hamP105; hamP106; hamP107; hamP108; hamP109; hamP110; hamP111; hamP112; hamP113; hamP114; hamP115; hamP116; hamP117; hamP118; hamP119; hamP120; hamP121; hamP122; hamP123; hamP124; hamP125; hamP126; hamP20; hamP21; hamP22; hamP23; hamP24; hamP25; hamP26; hamP93; hamP94; hamP95; hamP96; hamP97; hamP98; hamP99; intM0; intM100; intM101; intM102; intM103; intM104; intM105; intM106; intM107; intM108; intM109; intM110; intM111; intM112; intM113; intM114; intM115; intM116; intM117; intM118; intM119; intM120; intM121; intM122; intM123; intM124; intM125; intM126; intM127; intM50; intM51; intM52; intM53; intM54; intM55; intM56; intM57; intM58; intM59; intM60; intM61; intM62; intM63; intM64; intM65; intM66; intM67; intM68; intM69; intM70; intM71; intM72; intM73; intM74; intM75; intM76; intM77; intM78; intM79; intM80; intM81; intM82; intM83; intM84; intM85; intM86; intM87; intM88; intM89; intM90; intM91; intM92; intM93; intM94; intM95; intM96; intM97; intM98; intM99; intP0; intP1; intP10; intP100; intP101; intP102; intP103; intP104; intP105; intP106; intP107; intP108; intP109; intP11; intP110; intP111; intP112; intP113; intP114; intP115; intP116; intP117; intP118; intP119; intP12; intP120; intP121; intP122; intP123; intP124; intP125; intP126; intP127; intP13; intP14; intP15; intP16; intP17; intP18; intP19; intP2; intP20; intP21; intP22; intP23; intP24; intP25; intP26; intP3; intP4; intP5; intP6; intP7; intP8; intP9; intP94; intP95; intP96; intP97; intP98; intP99; rickM0; rickM102; rickM103; rickM104; rickM105; rickM106; rickM107; rickM108; rickM109; rickM110; rickM111; rickM112; rickM113; rickM114; rickM115; rickM116; rickM117; rickM118; rickM119; rickM120; rickM121; rickM122; rickM123; rickM124; rickM125; rickM126; rickM127; rickM49; rickM50; rickM51; rickM52; rickM53; rickM54; rickM55; rickM56; rickM57; rickM58; rickM59; rickM60; rickM61; rickM62; rickM63; rickM64; rickM65; rickM66; rickM67; rickM68; rickM69; rickM70; rickM71; rickM72; rickM73; rickM74; rickM75; rickP0; rickP1; rickP10; rickP106; rickP107; rickP108; rickP109; rickP11; rickP110; rickP111; rickP112; rickP113; rickP114; rickP115; rickP116; rickP117; rickP118; rickP119; rickP12; rickP120; rickP121; rickP122; rickP123; rickP124; rickP125; rickP126; rickP127; rickP2; rickP3; rickP4; rickP5; rickP6; rickP7; rickP8; rickP9; nosound; nosound.
    { 0x4FFEE03,0x0FFE804, 0x80,0x00, 0xC,+0 }, // 223: hamM1; intM1; rickM1; DBlock; DBlock.i
    { 0x122F603,0x0F8F3A1, 0x87,0x80, 0x6,+0 }, // 224: hamM2; intM2; rickM2; GClean; GClean.i
    { 0x007A810,0x005FA00, 0x86,0x03, 0x6,+0 }, // 225: hamM4; intM4; rickM4; DToms; DToms.in
    { 0x053F131,0x227F232, 0x48,0x00, 0x6,+0 }, // 226: f53GM63; hamM7; intM7; rickM7; rickM84; GOverD; GOverD.i; Guit_fz2; Synth Brass 2
    { 0x01A9161,0x01AC1E6, 0x40,0x03, 0x8,+0 }, // 227: hamM8; intM8; rickM8; GMetal; GMetal.i
    { 0x071FB11,0x0B9F301, 0x00,0x00, 0x0,+0 }, // 228: hamM9; intM9; rickM9; BPick; BPick.in
    { 0x1B57231,0x098D523, 0x0B,0x00, 0x8,+0 }, // 229: hamM10; intM10; rickM10; BSlap; BSlap.in
    { 0x024D501,0x0228511, 0x0F,0x00, 0xA,+0 }, // 230: hamM11; intM11; rickM11; BSynth1; BSynth1.
    { 0x025F911,0x034F131, 0x05,0x00, 0xA,+0 }, // 231: hamM12; intM12; rickM12; BSynth2; BSynth2.
    { 0x01576A1,0x0378261, 0x94,0x00, 0xC,+0 }, // 232: hamM15; intM15; rickM15; PSoft; PSoft.in
    { 0x1362261,0x0084F22, 0x10,0x40, 0x8,+0 }, // 233: hamM18; intM18; rickM18; PRonStr1
    { 0x2363360,0x0084F22, 0x15,0x40, 0xC,+0 }, // 234: hamM19; intM19; rickM19; PRonStr2
    { 0x007F804,0x0748201, 0x0E,0x05, 0x6,+0 }, // 235: hamM25; intM25; rickM25; LTrap; LTrap.in
    { 0x0E5F131,0x174F131, 0x89,0x00, 0xC,+0 }, // 236: hamM26; intM26; rickM26; LSaw; LSaw.ins
    { 0x0E3F131,0x073F172, 0x8A,0x00, 0xA,+0 }, // 237: hamM27; intM27; rickM27; PolySyn; PolySyn.
    { 0x0FFF101,0x0FF5091, 0x0D,0x80, 0x6,+0 }, // 238: hamM28; intM28; rickM28; Pobo; Pobo.ins
    { 0x1473161,0x007AF61, 0x0F,0x00, 0xA,+0 }, // 239: hamM29; intM29; rickM29; PSweep2; PSweep2.
    { 0x0D3B303,0x024F204, 0x40,0x80, 0x4,+0 }, // 240: hamM30; intM30; rickM30; LBright; LBright.
    { 0x1037531,0x0445462, 0x1A,0x40, 0xE,+0 }, // 241: hamM31; intM31; rickM31; SynStrin
    { 0x021A1A1,0x116C261, 0x92,0x40, 0x6,+0 }, // 242: hamM32; intM32; rickM32; SynStr2; SynStr2.
    { 0x0F0F240,0x0F4F440, 0x00,0x00, 0x4,+0 }, // 243: hamM33; intM33; rickM33; low_blub
    { 0x003F1C0,0x001107E, 0x4F,0x0C, 0x2,+0 }, // 244: hamM34; intM34; rickM34; DInsect; DInsect.
    { 0x0459BC0,0x015F9C1, 0x05,0x00, 0xE,+0 }, // 245: f25GM0; hamM35; intM35; rickM35; AcouGrandPiano; hardshak
    { 0x0064F50,0x003FF50, 0x10,0x00, 0x0,+0 }, // 246: hamM37; intM37; rickM37; WUMP; WUMP.ins
    { 0x2F0F005,0x1B4F600, 0x08,0x00, 0xC,+0 }, // 247: hamM38; intM38; rickM38; DSnare; DSnare.i
    { 0x0F2F931,0x042F210, 0x40,0x00, 0x4,+0 }, // 248: f25GM112; hamM39; intM39; rickM39; DTimp; DTimp.in; Tinkle Bell
    { 0x00FFF7E,0x00F2F6E, 0x00,0x00, 0xE,+0 }, // 249: hamM40; intM40; intP93; rickM40; DRevCym; DRevCym.; drevcym
    { 0x2F95401,0x2FB5401, 0x19,0x00, 0x8,+0 }, // 250: hamM41; intM41; rickM41; Dorky; Dorky.in
    { 0x0665F53,0x0077F00, 0x05,0x00, 0x6,+0 }, // 251: hamM42; intM42; rickM42; DFlab; DFlab.in
    { 0x003F1C0,0x006707E, 0x4F,0x03, 0x2,+0 }, // 252: hamM43; intM43; rickM43; DInsect2
    { 0x1111EF0,0x11411E2, 0x00,0xC0, 0x8,+0 }, // 253: hamM44; intM44; rickM44; DChopper
    { 0x0F0A006,0x075C584, 0x00,0x00, 0xE,+0 }, // 254: hamM45; hamP50; intM45; intP50; rickM45; rickP50; DShot; DShot.in; shot; shot.ins
    { 0x1F5F213,0x0F5F111, 0xC6,0x05, 0x0,+0 }, // 255: hamM46; intM46; rickM46; KickAss; KickAss.
    { 0x153F101,0x274F111, 0x49,0x02, 0x6,+0 }, // 256: hamM47; intM47; rickM47; RVisCool
    { 0x0E4F4D0,0x006A29E, 0x80,0x00, 0x8,+0 }, // 257: hamM48; intM48; rickM48; DSpring; DSpring.
    { 0x0871321,0x0084221, 0xCD,0x80, 0x8,+0 }, // 258: intM49; Chorar22
    { 0x005F010,0x004D011, 0x25,0x80, 0xE,+0 }, // 259: hamP27; intP27; rickP27; timpani; timpani.
    { 0x065B400,0x075B400, 0x00,0x00, 0x7,+0 }, // 260: hamP28; intP28; rickP28; timpanib
    { 0x02AF800,0x145F600, 0x03,0x00, 0x4,+0 }, // 261: f15GP41; f15GP43; f15GP45; f15GP47; f15GP48; f15GP50; f26GP41; f26GP43; f26GP45; f26GP47; f26GP48; f26GP50; hamP29; intP29; rickP29; APS043; APS043.i; High Floor Tom; High Tom; High-Mid Tom; Low Floor Tom; Low Tom; Low-Mid Tom
    { 0x0FFF830,0x07FF511, 0x44,0x00, 0x8,+0 }, // 262: hamP30; intP30; rickP30; mgun3; mgun3.in
    { 0x0F9F900,0x023F110, 0x08,0x00, 0xA,+0 }, // 263: hamP31; intP31; rickP31; kick4r; kick4r.i
    { 0x0F9F900,0x026F180, 0x04,0x00, 0x8,+0 }, // 264: hamP32; intP32; rickP32; timb1r; timb1r.i
    { 0x1FDF800,0x059F800, 0xC4,0x00, 0xA,+0 }, // 265: hamP33; intP33; rickP33; timb2r; timb2r.i
    { 0x06FFA00,0x08FF600, 0x0B,0x00, 0x0,+0 }, // 266: hamP36; intP36; rickP36; hartbeat
    { 0x0F9F900,0x023F191, 0x04,0x00, 0x8,+0 }, // 267: hamP37; hamP38; hamP39; hamP40; intP37; intP38; intP39; intP40; rickP37; rickP38; rickP39; rickP40; tom1r; tom1r.in
    { 0x097C802,0x097C802, 0x00,0x00, 0x1,+0 }, // 268: hamP71; intP41; intP42; intP43; intP44; intP71; rickP41; rickP42; rickP43; rickP44; rickP71; tom2; tom2.ins; woodbloc
    { 0x0BFFA01,0x0BFDA02, 0x00,0x00, 0x8,+0 }, // 269: MGM113; MGP37; MGP41; MGP43; MGP45; MGP47; MGP48; MGP50; MGP56; MGP60; MGP61; MGP62; MGP63; MGP64; MGP65; MGP66; b41P40; b41P41; b41P43; b41P45; b41P50; b41P56; b41P61; b41P62; b41P64; b41P65; b41P66; f19GP37; f19GP40; f19GP41; f19GP43; f19GP45; f19GP50; f19GP56; f19GP61; f19GP62; f19GP64; f19GP65; f19GP66; f21GP41; f21GP43; f21GP45; f21GP50; f21GP56; f21GP61; f21GP62; f21GP64; f21GP65; f21GP66; f23GP41; f23GP43; f23GP45; f23GP47; f23GP48; f23GP50; f23GP60; f23GP61; f23GP62; f23GP63; f23GP64; f23GP65; f23GP66; f32GM113; f32GP37; f32GP41; f32GP43; f32GP45; f32GP47; f32GP48; f32GP50; f32GP56; f32GP60; f32GP61; f32GP62; f32GP63; f32GP64; f32GP65; f32GP66; f41GP40; f41GP41; f41GP43; f41GP45; f41GP50; f41GP56; f41GP61; f41GP62; f41GP64; f41GP65; f41GP66; hamP45; intP45; rickP45; Agogo Bells; Cow Bell; Electric Snare; High Bongo; High Floor Tom; High Timbale; High Tom; High-Mid Tom; Low Bongo; Low Conga; Low Floor Tom; Low Timbale; Low Tom; Low-Mid Tom; Mute High Conga; Open High Conga; Side Stick; tom; tom.ins
    { 0x2F0FB01,0x096F701, 0x10,0x00, 0xE,+0 }, // 270: f25GM113; hamP46; hamP47; intP46; intP47; rickP46; rickP47; Agogo Bells; conga; conga.in
    { 0x002FF04,0x007FF00, 0x00,0x00, 0xE,+0 }, // 271: hamP48; intP48; rickP48; snare01r
    { 0x0F0F006,0x0B7F600, 0x00,0x00, 0xC,+0 }, // 272: hamP49; intP49; rickP49; slap; slap.ins
    { 0x0F0F006,0x034C4C4, 0x00,0x03, 0xE,+0 }, // 273: f47GP71; f47GP72; hamP51; intP51; rickP51; Long Whistle; Short Whistle; snrsust; snrsust.
    { 0x0F0F019,0x0F7B720, 0x0E,0x0A, 0xE,+0 }, // 274: intP52; rickP52; snare; snare.in
    { 0x0F0F006,0x0B4F600, 0x00,0x00, 0xE,+0 }, // 275: MGM114; MGM117; MGM118; b41M117; b41M118; f19GM114; f19GM117; f19GM118; f21GM117; f21GM118; f32GM114; f32GM117; f32GM118; f35GM118; f41GM114; f41GM117; f41GM118; f47GP69; f47GP70; f53GM114; hamP53; intP53; oGM118; rickP53; Cabasa; Maracas; Melodic Tom; Steel Drums; Synth Drum; synsnar; synsnar.; synsnr2.
    { 0x0F0F006,0x0B6F800, 0x00,0x00, 0xE,+0 }, // 276: MGM115; MGM116; b41M116; f19GM116; f21GM116; f32GM115; f32GM116; f41GM116; f53GM115; hamP54; intP54; rickP54; Taiko Drum; Woodblock; synsnr1; synsnr1.
    { 0x0F2F931,0x008F210, 0x40,0x00, 0x4,+0 }, // 277: hamP56; intP56; rickP56; rimshotb
    { 0x0BFFA01,0x0BFDA09, 0x00,0x08, 0x8,+0 }, // 278: hamP57; intP57; rickP57; rimshot; rimshot.
    { 0x210BA2E,0x2F4B40E, 0x0E,0x00, 0xE,+0 }, // 279: hamP58; hamP59; intP58; intP59; rickP58; rickP59; crash; crash.in
    { 0x210FA2E,0x2F4F40E, 0x0E,0x00, 0xE,+0 }, // 280: f25GM125; f25GP49; intP60; rickP60; Crash Cymbal 1; Helicopter; cymbal; cymbal.i
    { 0x2A2B2A4,0x1D49703, 0x02,0x80, 0xE,+0 }, // 281: b41M119; f21GM119; f32GM119; f41GM119; f47GP28; f53GM117; hamP60; hamP61; intP61; rickP61; Melodic Tom; Reverse Cymbal; cymbal; cymbal.i; cymbals; cymbals.
    { 0x200FF04,0x206FFC3, 0x00,0x00, 0x8,+0 }, // 282: hamP62; intP62; rickP62; hammer5r
    { 0x200FF04,0x2F5F6C3, 0x00,0x00, 0x8,+0 }, // 283: hamP63; hamP64; intP63; intP64; rickP63; rickP64; hammer3; hammer3.
    { 0x0E1C000,0x153951E, 0x80,0x80, 0x6,+0 }, // 284: hamP65; intP65; rickP65; ride2; ride2.in
    { 0x200FF03,0x3F6F6C4, 0x00,0x00, 0x8,+0 }, // 285: hamP66; intP66; rickP66; hammer1; hammer1.
    { 0x202FF4E,0x3F7F701, 0x00,0x00, 0x8,+0 }, // 286: intP67; intP68; rickP67; rickP68; tambour; tambour.
    { 0x202FF4E,0x3F6F601, 0x00,0x00, 0x8,+0 }, // 287: hamP69; hamP70; intP69; intP70; rickP69; rickP70; tambou2; tambou2.
    { 0x2588A51,0x018A452, 0x00,0x00, 0xC,+0 }, // 288: hamP72; intP72; rickP72; woodblok
    { 0x0FFFB13,0x0FFE808, 0x40,0x00, 0x8,+0 }, // 289: intP73; intP74; rickP73; rickP74; claves; claves.i
    { 0x0FFEE03,0x0FFE808, 0x40,0x00, 0xC,+0 }, // 290: hamP75; intP75; rickP75; claves2; claves2.
    { 0x0FFEE05,0x0FFE808, 0x55,0x00, 0xE,+0 }, // 291: hamP76; intP76; rickP76; claves3; claves3.
    { 0x0FF0006,0x0FDF715, 0x3F,0x0D, 0x1,+0 }, // 292: hamP77; intP77; rickP77; clave; clave.in
    { 0x0F6F80E,0x0F6F80E, 0x00,0x00, 0x0,+0 }, // 293: hamP78; hamP79; intP78; intP79; rickP78; rickP79; agogob4; agogob4.
    { 0x060F207,0x072F212, 0x4F,0x09, 0x8,+0 }, // 294: hamP80; intP80; rickP80; clarion; clarion.
    { 0x061F217,0x074F212, 0x4F,0x08, 0x8,+0 }, // 295: MGM38; f23GM38; f32GM38; hamP81; intP81; rickP81; Synth Bass 1; trainbel
    { 0x022FB18,0x012F425, 0x88,0x80, 0x8,+0 }, // 296: hamP82; intP82; rickP82; gong; gong.ins
    { 0x0F0FF04,0x0B5F4C1, 0x00,0x00, 0xE,+0 }, // 297: hamP83; intP83; rickP83; kalimbai
    { 0x02FC811,0x0F5F531, 0x2D,0x00, 0xC,+0 }, // 298: MGM103; f32GM103; hamP84; hamP85; intP84; intP85; oGM103; rickP84; rickP85; FX 8 sci-fi; xylo1; xylo1.in
    { 0x03D6709,0x3FC692C, 0x00,0x00, 0xE,+0 }, // 299: hamP86; intP86; rickP86; match; match.in
    { 0x053D144,0x05642B2, 0x80,0x15, 0xE,+0 }, // 300: hamP87; intP87; rickP87; breathi; breathi.
    { 0x0F0F007,0x0DC5C00, 0x00,0x00, 0xE,+0 }, // 301: f12GM29; f12GM30; f12GM31; f12GM44; f12GM50; f12GM51; f12GM52; f12GM53; f12GM54; f12GM55; f12GM65; f12GM66; f12GM67; f12GM75; f12GP51; f16GM29; f16GM30; f16GM31; f16GM44; f16GM50; f16GM51; f16GM52; f16GM53; f16GM54; f16GM55; f16GM65; f16GM66; f16GM67; f16GM75; f16GP51; f54GM29; f54GM30; f54GM31; f54GM44; f54GM50; f54GM51; f54GM52; f54GM53; f54GM54; f54GM55; f54GM65; f54GM66; f54GM67; f54GP51; intP88; rickP88; Alto Sax; Baritone Sax; Choir Aahs; Distorton Guitar; Guitar Harmonics; Orchestra Hit; Overdrive Guitar; Pan Flute; Ride Cymbal 1; Synth Strings 1; Synth Voice; SynthStrings 2; Tenor Sax; Tremulo Strings; Voice Oohs; scratch; scratch.
    { 0x00FFF7E,0x00F3F6E, 0x00,0x00, 0xE,+0 }, // 302: hamP89; intP89; rickP89; crowd; crowd.in
    { 0x0B3FA00,0x005D000, 0x00,0x00, 0xC,+0 }, // 303: intP90; rickP90; taiko; taiko.in
    { 0x0FFF832,0x07FF511, 0x84,0x00, 0xE,+0 }, // 304: hamP91; intP91; rickP91; rlog; rlog.ins
    { 0x0089FD4,0x0089FD4, 0xC0,0xC0, 0x4,+0 }, // 305: hamP92; intP92; rickP92; knock; knock.in
    { 0x253B1C4,0x083B1D2, 0x8F,0x84, 0x2,+0 }, // 306: f35GM88; hamM52; rickM94; Fantasy1; Pad 1 new age; fantasy1
    { 0x175F5C2,0x074F2D1, 0x21,0x83, 0xE,+0 }, // 307: f35GM24; hamM53; Acoustic Guitar1; guitar1
    { 0x1F6FB34,0x04394B1, 0x83,0x00, 0xC,+0 }, // 308: hamM55; hamatmos
    { 0x0658722,0x0186421, 0x46,0x80, 0x0,+0 }, // 309: f35GM82; hamM56; Lead 3 calliope; hamcalio
    { 0x0BDF211,0x09BA004, 0x46,0x40, 0x8,+0 }, // 310: MGM37; b41M37; f19GM37; f23GM37; f32GM37; f35GM84; f41GM37; hamM59; oGM37; Lead 5 charang; Slap Bass 2; moon; moon.ins
    { 0x144F221,0x3457122, 0x8A,0x40, 0x0,+0 }, // 311: hamM62; Polyham3
    { 0x144F221,0x1447122, 0x8A,0x40, 0x0,+0 }, // 312: hamM63; Polyham
    { 0x053F101,0x153F108, 0x40,0x40, 0x0,+0 }, // 313: f35GM104; hamM64; Sitar; sitar2
    { 0x102FF00,0x3FFF200, 0x08,0x00, 0x0,+0 }, // 314: f35GM81; hamM70; Lead 2 sawtooth; weird1a
    { 0x144F221,0x345A122, 0x8A,0x40, 0x0,+0 }, // 315: hamM71; Polyham4
    { 0x028F131,0x018F031, 0x0F,0x00, 0xA,+0 }, // 316: hamM72; hamsynbs
    { 0x307D7E1,0x107B6E0, 0x8D,0x00, 0x1,+0 }, // 317: hamM73; Ocasynth
    { 0x03DD500,0x02CD500, 0x11,0x00, 0xA,+0 }, // 318: hamM120; hambass1
    { 0x1199563,0x219C420, 0x46,0x00, 0x8,+0 }, // 319: hamM121; hamguit1
    { 0x044D08C,0x2F4D181, 0xA1,0x80, 0x8,+0 }, // 320: hamM122; hamharm2
    { 0x0022171,0x1035231, 0x93,0x80, 0x0,+0 }, // 321: hamM123; hamvox1
    { 0x1611161,0x01311A2, 0x91,0x80, 0x8,+0 }, // 322: hamM124; hamgob1
    { 0x25666E1,0x02665A1, 0x4C,0x00, 0x0,+0 }, // 323: hamM125; hamblow1
    { 0x144F5C6,0x018F6C1, 0x5C,0x83, 0xE,+0 }, // 324: f35GP56; hamP9; Cow Bell; cowbell
    { 0x0D0CCC0,0x028EAC1, 0x10,0x00, 0x0,+0 }, // 325: f35GP62; hamP10; rickP100; Mute High Conga; conghi; conghi.i
    { 0x038FB00,0x0DAF400, 0x00,0x00, 0x4,+0 }, // 326: f35GP36; hamP12; rickP15; Bass Drum 1; hamkick; kick3.in
    { 0x2BFFB15,0x31FF817, 0x0A,0x00, 0x0,+0 }, // 327: f35GP37; hamP13; Side Stick; rimshot2
    { 0x0F0F01E,0x0B6F70E, 0x00,0x00, 0xE,+0 }, // 328: f35GP38; hamP14; rickP16; Acoustic Snare; hamsnr1; snr1.ins
    { 0x0BFFBC6,0x02FE8C9, 0x00,0x00, 0xE,+0 }, // 329: f35GP39; hamP15; Hand Clap; handclap
    { 0x2F0F006,0x2B7F800, 0x00,0x00, 0xE,+0 }, // 330: f35GP40; hamP16; Electric Snare; smallsnr
    { 0x0B0900E,0x0BF990E, 0x03,0x03, 0xA,+0 }, // 331: f35GP42; hamP17; rickP95; Closed High Hat; clsdhhat
    { 0x283E0C4,0x14588C0, 0x81,0x00, 0xE,+0 }, // 332: f35GP46; hamP18; rickP94; Open High Hat; openhht2
    { 0x097C802,0x040E000, 0x00,0x00, 0x1,+0 }, // 333: hamP41; hamP42; hamP43; hamP44; tom2
    { 0x00FFF2E,0x04AF602, 0x0A,0x1B, 0xE,+0 }, // 334: hamP52; snare
    { 0x3A5F0EE,0x36786CE, 0x00,0x00, 0xE,+0 }, // 335: f35GP54; hamP67; hamP68; Tambourine; tambour
    { 0x0B0FCD6,0x008BDD6, 0x00,0x05, 0xA,+0 }, // 336: f35GP75; hamP73; hamP74; Claves; claves
    { 0x0F0F007,0x0DC5C00, 0x08,0x00, 0xE,+0 }, // 337: hamP88; scratch
    { 0x0E7F301,0x078F211, 0x58,0x00, 0xA,+0 }, // 338: rickM76; Bass.ins
    { 0x0EFF230,0x078F521, 0x1E,0x00, 0xE,+0 }, // 339: f35GM36; rickM77; Basnor04; Slap Bass 1
    { 0x019D530,0x01B6171, 0x88,0x80, 0xC,+0 }, // 340: b41M39; f32GM39; f41GM39; rickM78; Synbass1; Synth Bass 2; synbass1
    { 0x001F201,0x0B7F211, 0x0D,0x0D, 0xA,+0 }, // 341: rickM79; Synbass2
    { 0x03DD500,0x02CD500, 0x14,0x00, 0xA,+0 }, // 342: f35GM34; rickM80; Electric Bass 2; Pickbass
    { 0x010E032,0x0337D16, 0x87,0x84, 0x8,+0 }, // 343: rickM82; Harpsi1.
    { 0x0F8F161,0x008F062, 0x80,0x80, 0x8,+0 }, // 344: rickM83; Guit_el3
    { 0x0745391,0x0755451, 0x00,0x00, 0xA,+0 }, // 345: rickM88; Orchit2.
    { 0x08E6121,0x09E7231, 0x15,0x00, 0xE,+0 }, // 346: f35GM61; rickM89; Brass Section; Brass11.
    { 0x0BC7321,0x0BC8121, 0x19,0x00, 0xE,+0 }, // 347: f12GM62; f16GM62; f47GM61; f53GM89; f54GM62; rickM90; Brass Section; Brass2.i; Pad 2 warm; Synth Brass 1
    { 0x23C7320,0x0BC8121, 0x19,0x00, 0xE,+0 }, // 348: f12GM61; f16GM61; f37GM61; f47GM63; f54GM61; rickM91; Brass Section; Brass3.i; Synth Brass 2
    { 0x209A060,0x20FF014, 0x02,0x80, 0x1,+0 }, // 349: rickM92; Squ_wave
    { 0x064F207,0x075F612, 0x73,0x00, 0x8,+0 }, // 350: rickM99; Agogo.in
    { 0x2B7F811,0x006F311, 0x46,0x00, 0x8,+0 }, // 351: oGM113; oGP35; oGP36; rickP13; Ac Bass Drum; Agogo Bells; Bass Drum 1; kick1.in
    { 0x218F401,0x008F800, 0x00,0x00, 0xC,+0 }, // 352: rickP17; rickP19; snare1.i; snare4.i
    { 0x0F0F009,0x0F7B700, 0x0E,0x00, 0xE,+0 }, // 353: rickP18; rickP20; snare2.i; snare5.i
    { 0x0FEF812,0x07ED511, 0x47,0x00, 0xE,+0 }, // 354: rickP21; rickP22; rickP23; rickP24; rickP25; rickP26; rocktom.
    { 0x2F4F50E,0x424120CA, 0x00,0x51, 0x3,+0 }, // 355: rickP93; openhht1
    { 0x0DFDCC2,0x026C9C0, 0x17,0x00, 0x0,+0 }, // 356: f35GP74; rickP97; Long Guiro; guirol.i
    { 0x0D0ACC0,0x028EAC1, 0x18,0x00, 0x0,+0 }, // 357: f35GP63; f35GP64; rickP101; rickP102; Low Conga; Open High Conga; congas2.
    { 0x0A7CDC2,0x028EAC1, 0x2B,0x02, 0x0,+0 }, // 358: f35GP60; f35GP61; rickP103; rickP104; High Bongo; Low Bongo; bongos.i
    { 0x1F3F030,0x1F4F130, 0x54,0x00, 0xA,+12 }, // 359: dMM0; hxMM0; Acoustic Grand Piano
    { 0x0F3F030,0x1F4F130, 0x52,0x00, 0xA,+12 }, // 360: dMM1; hxMM1; Bright Acoustic Piano
    { 0x1F3E130,0x0F4F130, 0x4E,0x00, 0x8,+12 }, // 361: dMM2; hxMM2; Electric Grand Piano
    { 0x015E811,0x014F712, 0x00,0x00, 0x1,+12 }, // 362: dMM2; hxMM2; Electric Grand Piano
    { 0x153F110,0x0F4D110, 0x4F,0x00, 0x6,+12 }, // 363: dMM3; hxMM3; Honky-tonk Piano
    { 0x053F111,0x0F4D111, 0x4F,0x00, 0x6,+12 }, // 364: dMM3; hxMM3; Honky-tonk Piano
    { 0x051F121,0x0E5D231, 0x66,0x00, 0x6,+0 }, // 365: dMM4; hxMM4; Rhodes Paino
    { 0x0E6F130,0x0E5F1B0, 0x51,0x40, 0x6,+12 }, // 366: dMM5; hxMM5; Chorused Piano
    { 0x079F212,0x099F110, 0x43,0x40, 0x9,+12 }, // 367: dMM5; hxMM5; Chorused Piano
    { 0x201F230,0x1F4C130, 0x87,0x00, 0x6,+12 }, // 368: dMM6; hxMM6; Harpsichord
    { 0x162A190,0x1A79110, 0x8E,0x00, 0xC,+12 }, // 369: dMM7; hxMM7; Clavinet
    { 0x164F228,0x0E4F231, 0x4F,0x00, 0x8,+0 }, // 370: dMM8; hxMM8; Celesta
    { 0x0119113,0x0347D14, 0x0E,0x00, 0x9,+0 }, // 371: dMM9; hxMM9; * Glockenspiel
    { 0x041F6B2,0x092D290, 0x0F,0x00, 0x0,+12 }, // 372: dMM10; hxMM10; * Music Box
    { 0x0F3F1F0,0x0F4F1F2, 0x02,0x00, 0x1,+12 }, // 373: dMM11; hxMM11; Vibraphone
    { 0x0157980,0x275F883, 0x00,0x00, 0x1,+12 }, // 374: dMM12; hxMM12; Marimba
    { 0x093F614,0x053F610, 0x1F,0x00, 0x8,+12 }, // 375: dMM13; hxMM13; Xylophone
    { 0x113B681,0x013FF02, 0x99,0x00, 0xA,+0 }, // 376: dMM14; hxMM14; * Tubular-bell
    { 0x0119130,0x0535211, 0x47,0x80, 0x8,+12 }, // 377: dMM15; hxMM15; * Dulcimer
    { 0x016B1A0,0x117D161, 0x88,0x80, 0x7,+12 }, // 378: dMM16; hxMM16; Hammond Organ
    { 0x105F130,0x036F494, 0x00,0x00, 0x7,+0 }, // 379: dMM17; hxMM17; Percussive Organ
    { 0x017F2E2,0x107FF60, 0x9E,0x80, 0x0,+0 }, // 380: dMM18; hxMM18; Rock Organ
    { 0x117F2E0,0x007FFA0, 0x9E,0x80, 0x0,+12 }, // 381: dMM18; hxMM18; Rock Organ
    { 0x0043030,0x1145431, 0x92,0x80, 0x9,+12 }, // 382: dMM19; hxMM19; Church Organ
    { 0x0178000,0x1176081, 0x49,0x80, 0x6,+12 }, // 383: dMM20; hxMM20; Reed Organ
    { 0x015A220,0x1264131, 0x48,0x00, 0xA,+12 }, // 384: dMM21; hxMM21; Accordion
    { 0x0158220,0x1264631, 0x4A,0x00, 0xA,+12 }, // 385: dMM21; hxMM21; Accordion
    { 0x03460B0,0x01642B2, 0x0C,0x80, 0x8,+12 }, // 386: dMM22; hxMM22; Harmonica
    { 0x105F020,0x2055231, 0x92,0x00, 0x8,+12 }, // 387: dMM23; hxMM23; Tango Accordion
    { 0x105F020,0x2055231, 0x92,0x00, 0x0,+12 }, // 388: dMM23; hxMM23; Tango Accordion
    { 0x0F5F120,0x0F6F120, 0x8D,0x00, 0x0,+12 }, // 389: dMM24; hxMM24; Acoustic Guitar (nylon)
    { 0x1E4E130,0x0E3F230, 0x0D,0x00, 0xA,+12 }, // 390: dMM25; hxMM25; Acoustic Guitar (steel)
    { 0x21FF100,0x088F400, 0x21,0x00, 0xA,+12 }, // 391: dMM26; hxMM26; Electric Guitar (jazz)
    { 0x132EA10,0x2E7D210, 0x87,0x00, 0x2,+12 }, // 392: dMM27; hxMM27; * Electric Guitar (clean)
    { 0x0F4E030,0x0F5F230, 0x92,0x80, 0x0,+12 }, // 393: dMM28; hxMM28; Electric Guitar (muted)
    { 0x0FFF100,0x1FFF051, 0x10,0x00, 0xA,+12 }, // 394: dMM29; Overdriven Guitar
    { 0x0FFF110,0x1FFF051, 0x0D,0x00, 0xC,+12 }, // 395: dMM30; Distortion Guitar
    { 0x297A110,0x0E7E111, 0x43,0x00, 0x0,+12 }, // 396: dMM31; hxMM31; * Guitar Harmonics
    { 0x020C420,0x0F6C3B0, 0x0E,0x00, 0x0,+12 }, // 397: dMM32; Acoustic Bass
    { 0x0FFF030,0x0F8F131, 0x96,0x00, 0xA,+12 }, // 398: dMM33; hxMM33; Electric Bass (finger)
    { 0x014E020,0x0D6E130, 0x8F,0x80, 0x8,+12 }, // 399: dMM34; hxMM34; Electric Bass (pick)
    { 0x14551E1,0x14691A0, 0x4D,0x00, 0x0,+0 }, // 400: dMM35; Fretless Bass
    { 0x14551A1,0x14681A0, 0x4D,0x00, 0x0,+12 }, // 401: dMM35; dMM94; hxMM94; Fretless Bass
    { 0x2E7F030,0x047F131, 0x00,0x00, 0x0,+0 }, // 402: dMM36; * Slap Bass 1
    { 0x0E5F030,0x0F5F131, 0x90,0x80, 0x8,+12 }, // 403: dMM37; hxMM37; Slap Bass 2
    { 0x1F5F430,0x0F6F330, 0x0A,0x00, 0xA,+12 }, // 404: dMM38; hxMM38; Synth Bass 1
    { 0x1468330,0x017D231, 0x15,0x00, 0xA,+12 }, // 405: dMM39; hxMM39; Synth Bass 2
    { 0x1455060,0x14661A1, 0x17,0x00, 0x6,+12 }, // 406: dMM40; hxMM40; Violin
    { 0x04460F0,0x0154171, 0x8F,0x00, 0x2,+12 }, // 407: dMM41; hxMM41; Viola
    { 0x214D0B0,0x1176261, 0x0F,0x80, 0x6,+0 }, // 408: dMM42; hxMM42; Cello
    { 0x211B1F0,0x115A020, 0x8A,0x80, 0x6,+12 }, // 409: dMM43; hxMM43; Contrabass
    { 0x201C3F0,0x0058361, 0x89,0x40, 0x6,+0 }, // 410: dMM44; hxMM44; Tremolo Strings
    { 0x201B370,0x1059360, 0x89,0x40, 0x6,+12 }, // 411: dMM44; hxMM44; Tremolo Strings
    { 0x2F9F830,0x0E67620, 0x97,0x00, 0xE,+12 }, // 412: dMM45; hxMM45; Pizzicato Strings
    { 0x035F131,0x0B3F320, 0x24,0x00, 0x0,+12 }, // 413: dMM46; hxMM46; Orchestral Harp
    { 0x0C8AA00,0x0B3D210, 0x04,0x00, 0xA,+12 }, // 414: dMM47; hxMM47; * Timpani
    { 0x104C060,0x10455B1, 0x51,0x80, 0x4,+12 }, // 415: dMM48; hxMM48; String Ensemble 1
    { 0x10490A0,0x1045531, 0x52,0x80, 0x6,+12 }, // 416: dMM48; hxMM48; String Ensemble 1
    { 0x1059020,0x10535A1, 0x51,0x80, 0x4,+12 }, // 417: dMM49; hxMM49; String Ensemble 2
    { 0x10590A0,0x1053521, 0x52,0x80, 0x6,+12 }, // 418: dMM49; hxMM49; String Ensemble 2
    { 0x20569A1,0x20266F1, 0x93,0x00, 0xA,+0 }, // 419: dMM50; hxMM50; Synth Strings 1
    { 0x0031121,0x1043120, 0x4D,0x80, 0x0,+12 }, // 420: dMM51; hxMM51; Synth Strings 2
    { 0x2331100,0x1363100, 0x82,0x80, 0x8,+12 }, // 421: dMM51; hxMM51; Synth Strings 2
    { 0x0549060,0x0047060, 0x56,0x40, 0x0,+12 }, // 422: dMM52; hxMM52; Choir Aahs
    { 0x0549020,0x0047060, 0x92,0xC0, 0x0,+12 }, // 423: dMM52; hxMM52; Choir Aahs
    { 0x0B7B1A0,0x08572A0, 0x99,0x80, 0x0,+12 }, // 424: dMM53; hxMM53; Voice Oohs
    { 0x05460B0,0x07430B0, 0x5A,0x80, 0x0,+12 }, // 425: dMM54; hxMM54; Synth Voice
    { 0x0433010,0x0146410, 0x90,0x00, 0x2,-12 }, // 426: dMM55; hxMM55; Orchestra Hit
    { 0x0425090,0x0455411, 0x8F,0x00, 0x2,+0 }, // 427: dMM55; hxMM55; Orchestra Hit
    { 0x1158020,0x0365130, 0x8E,0x00, 0xA,+12 }, // 428: dMM56; hxMM56; Trumpet
    { 0x01F71B0,0x03B7220, 0x1A,0x80, 0xE,+12 }, // 429: dMM57; hxMM57; Trombone
    { 0x0468020,0x1569220, 0x16,0x00, 0xC,+12 }, // 430: dMM58; Tuba
    { 0x1E68080,0x1F65190, 0x8D,0x00, 0xC,+12 }, // 431: dMM59; hxMM59; Muted Trumpet
    { 0x0B87020,0x0966120, 0x22,0x80, 0xE,+12 }, // 432: dMM60; hxMM60; French Horn
    { 0x0B87020,0x0966120, 0x23,0x80, 0xE,+12 }, // 433: dMM60; hxMM60; French Horn
    { 0x1156020,0x0365130, 0x8E,0x00, 0xA,+12 }, // 434: dMM61; hxMM61; Brass Section
    { 0x1177030,0x1366130, 0x92,0x00, 0xE,+12 }, // 435: dMM61; hxMM61; Brass Section
    { 0x2A69120,0x1978120, 0x4D,0x00, 0xC,+12 }, // 436: dMM62; hxMM62; Synth Brass 1
    { 0x2A69120,0x1979120, 0x8C,0x00, 0xC,+12 }, // 437: dMM62; hxMM62; Synth Brass 1
    { 0x2A68130,0x1976130, 0x50,0x00, 0xC,+12 }, // 438: dMM63; hxMM63; Synth Bass 2
    { 0x2A68130,0x1976130, 0x4A,0x00, 0xA,+12 }, // 439: dMM63; hxMM63; Synth Bass 2
    { 0x00560A0,0x11652B1, 0x96,0x00, 0x6,+12 }, // 440: dMM64; hxMM64; Soprano Sax
    { 0x10670A0,0x11662B0, 0x89,0x00, 0x6,+12 }, // 441: dMM65; hxMM65; Alto Sax
    { 0x00B98A0,0x10B73B0, 0x4A,0x00, 0xA,+12 }, // 442: dMM66; hxMM66; Tenor Sax
    { 0x10B90A0,0x11B63B0, 0x85,0x00, 0xA,+12 }, // 443: dMM67; hxMM67; Baritone Sax
    { 0x0167070,0x0085CA2, 0x90,0x80, 0x6,+12 }, // 444: dMM68; hxMM68; Oboe
    { 0x007C820,0x1077331, 0x4F,0x00, 0xA,+12 }, // 445: dMM69; hxMM69; English Horn
    { 0x0199030,0x01B6131, 0x91,0x80, 0xA,+12 }, // 446: dMM70; hxMM70; Bassoon
    { 0x017A530,0x01763B0, 0x8D,0x80, 0x8,+12 }, // 447: dMM71; hxMM71; Clarinet
    { 0x08F6EF0,0x02A3570, 0x80,0x00, 0xE,+12 }, // 448: dMM72; hxMM72; Piccolo
    { 0x08850A0,0x02A5560, 0x93,0x80, 0x8,+12 }, // 449: dMM73; hxMM73; Flute
    { 0x0176520,0x02774A0, 0x0A,0x00, 0xB,+12 }, // 450: dMM74; hxMM74; Recorder
    { 0x12724B0,0x01745B0, 0x84,0x00, 0x9,+12 }, // 451: dMM75; hxMM75; Pan Flute
    { 0x00457E1,0x0375760, 0xAD,0x00, 0xE,+12 }, // 452: dMM76; hxMM76; Bottle Blow
    { 0x33457F1,0x05D67E1, 0x28,0x00, 0xE,+0 }, // 453: dMM77; hxMM77; * Shakuhachi
    { 0x00F31D0,0x0053270, 0xC7,0x00, 0xB,+12 }, // 454: dMM78; hxMM78; Whistle
    { 0x00551B0,0x0294230, 0xC7,0x00, 0xB,+12 }, // 455: dMM79; hxMM79; Ocarina
    { 0x15B5122,0x1256030, 0x52,0x00, 0x0,+12 }, // 456: dMM80; hxMM80; Lead 1 (square)
    { 0x15B9122,0x125F030, 0x4D,0x00, 0x0,+12 }, // 457: dMM80; hxMM80; Lead 1 (square)
    { 0x19BC120,0x165C031, 0x43,0x00, 0x8,+12 }, // 458: dMM81; hxMM81; Lead 2 (sawtooth)
    { 0x1ABB160,0x005F131, 0x41,0x00, 0x8,+12 }, // 459: dMM81; hxMM81; Lead 2 (sawtooth)
    { 0x33357F0,0x00767E0, 0x28,0x00, 0xE,+12 }, // 460: dMM82; hxMM82; Lead 3 (calliope)
    { 0x30457E0,0x04D67E0, 0x23,0x00, 0xE,+12 }, // 461: dMM83; hxMM83; Lead 4 (chiffer)
    { 0x304F7E0,0x04D87E0, 0x23,0x00, 0xE,+12 }, // 462: dMM83; hxMM83; Lead 4 (chiffer)
    { 0x10B78A1,0x12BF130, 0x42,0x00, 0x8,+12 }, // 463: dMM84; hxMM84; Lead 5 (charang)
    { 0x0558060,0x014F2E0, 0x21,0x00, 0x8,+12 }, // 464: dMM85; hxMM85; Lead 6 (voice)
    { 0x0559020,0x014A2A0, 0x21,0x00, 0x8,+12 }, // 465: dMM85; hxMM85; Lead 6 (voice)
    { 0x195C120,0x16370B0, 0x43,0x80, 0xA,+12 }, // 466: dMM86; hxMM86; Lead 7 (5th sawtooth)
    { 0x19591A0,0x1636131, 0x49,0x00, 0xA,+7 }, // 467: dMM86; hxMM86; Lead 7 (5th sawtooth)
    { 0x1075124,0x229FDA0, 0x40,0x00, 0x9,+0 }, // 468: dMM87; dMM88; hxMM87; hxMM88; * Lead 8 (bass & lead)
    { 0x0053280,0x0053360, 0xC0,0x00, 0x9,+12 }, // 469: dMM89; hxMM89; Pad 2 (warm)
    { 0x0053240,0x00533E0, 0x40,0x00, 0x9,+12 }, // 470: dMM89; hxMM89; Pad 2 (warm)
    { 0x2A5A1A0,0x196A1A0, 0x8F,0x00, 0xC,+12 }, // 471: dMM90; hxMM90; Pad 3 (polysynth)
    { 0x005F0E0,0x0548160, 0x44,0x00, 0x1,+12 }, // 472: dMM91; hxMM91; Pad 4 (choir)
    { 0x105F0E0,0x0547160, 0x44,0x80, 0x1,+12 }, // 473: dMM91; hxMM91; Pad 4 (choir)
    { 0x033A180,0x05452E0, 0x8A,0x00, 0x7,+12 }, // 474: dMM92; hxMM92; Pad 5 (bowed glass)
    { 0x1528081,0x1532340, 0x9D,0x80, 0xE,+12 }, // 475: dMM93; hxMM93; Pad 6 (metal)
    { 0x14551E1,0x14691A0, 0x4D,0x00, 0x0,+12 }, // 476: dMM94; hxMM94; Pad 7 (halo)
    { 0x15211E1,0x17380E0, 0x8C,0x80, 0x8,+12 }, // 477: dMM95; hxMM95; Pad 8 (sweep)
    { 0x0477220,0x019F883, 0x40,0x00, 0xB,+12 }, // 478: dMM96; hxMM96; FX 1 (rain)
    { 0x1028500,0x11245C1, 0xD2,0x00, 0xA,+0 }, // 479: dMM97; hxMM97; FX 2 (soundtrack)
    { 0x0034522,0x23535E3, 0xD2,0x00, 0xA,+7 }, // 480: dMM97; hxMM97; FX 2 (soundtrack)
    { 0x074F604,0x024A302, 0xC0,0x00, 0x0,-12 }, // 481: dMM98; hxMM98; * FX 3 (crystal)
    { 0x0D2C090,0x0D2D130, 0x8E,0x00, 0x0,+12 }, // 482: dMM99; hxMM99; FX 4 (atmosphere)
    { 0x0D2D090,0x0D2F130, 0x8E,0x00, 0x0,+12 }, // 483: dMM99; hxMM99; FX 4 (atmosphere)
    { 0x0F390D0,0x0F3C2C0, 0x12,0x00, 0x0,+12 }, // 484: dMM100; hxMM100; FX 5 (brightness)
    { 0x0F390D0,0x0F2C2C0, 0x12,0x80, 0x0,+12 }, // 485: dMM100; hxMM100; FX 5 (brightness)
    { 0x15213E0,0x21333F1, 0x1A,0x80, 0x0,+0 }, // 486: dMM101; hxMM101; FX 6 (goblin)
    { 0x0BA45E0,0x19132F0, 0x1A,0x00, 0x0,+12 }, // 487: dMM102; hxMM102; FX 7 (echo drops)
    { 0x1025810,0x0724202, 0x18,0x00, 0xA,+12 }, // 488: dMM103; hxMM103; * FX 8 (star-theme)
    { 0x0B36320,0x0B36324, 0x08,0x00, 0x2,+12 }, // 489: dMM104; hxMM104; Sitar
    { 0x0127730,0x1F4F310, 0x0D,0x00, 0x4,+12 }, // 490: dMM105; hxMM105; Banjo
    { 0x033F900,0x273F400, 0x80,0x80, 0x0,+12 }, // 491: dMM106; hxMM106; Shamisen
    { 0x2ACF907,0x229F90F, 0x1A,0x00, 0x0,+12 }, // 492: dMM106; hxMM106; Shamisen
    { 0x153F220,0x0E49122, 0x21,0x00, 0x8,+12 }, // 493: dMM107; hxMM107; Koto
    { 0x339F103,0x074D615, 0x4F,0x00, 0x6,+0 }, // 494: dMM108; hxMM108; Kalimba
    { 0x1158930,0x2076B21, 0x42,0x00, 0xA,+12 }, // 495: dMM109; hxMM109; Bag Pipe
    { 0x003A130,0x0265221, 0x1F,0x00, 0xE,+12 }, // 496: dMM110; hxMM110; Fiddle
    { 0x0134030,0x1166130, 0x13,0x80, 0x8,+12 }, // 497: dMM111; hxMM111; Shanai
    { 0x032A113,0x172B212, 0x00,0x80, 0x1,+5 }, // 498: dMM112; hxMM112; Tinkle Bell
    { 0x001E795,0x0679616, 0x81,0x00, 0x4,+12 }, // 499: dMM113; hxMM113; Agogo
    { 0x104F003,0x0058220, 0x49,0x00, 0x6,+12 }, // 500: dMM114; hxMM114; Steel Drums
    { 0x0D1F813,0x078F512, 0x44,0x00, 0x6,+12 }, // 501: dMM115; hxMM115; Woodblock
    { 0x0ECA710,0x0F5D510, 0x0B,0x00, 0x0,+0 }, // 502: dMM116; hxMM116; Taiko Drum
    { 0x0C8A820,0x0B7D601, 0x0B,0x00, 0x0,+0 }, // 503: dMM117; hxMM117; Melodic Tom
    { 0x0C4F800,0x0B7D300, 0x0B,0x00, 0x0,+12 }, // 504: dMM118; hxMM118; Synth Drum
    { 0x031410C,0x31D2110, 0x8F,0x80, 0xE,+0 }, // 505: dMM119; hxMM119; Reverse Cymbal
    { 0x1B33432,0x3F75431, 0x21,0x00, 0xE,+12 }, // 506: dMM120; hxMM120; Guitar Fret Noise
    { 0x00437D1,0x0343750, 0xAD,0x00, 0xE,+12 }, // 507: dMM121; hxMM121; Breath Noise
    { 0x2013E02,0x2F31408, 0x00,0x00, 0xE,+0 }, // 508: dMM122; hxMM122; Seashore
    { 0x003EBF5,0x06845F6, 0xD4,0x00, 0x7,+0 }, // 509: dMM123; hxMM123; Bird Tweet
    { 0x171DAF0,0x117B0CA, 0x00,0xC0, 0x8,+0 }, // 510: dMM124; hxMM124; Telephone Ring
    { 0x1111EF0,0x11121E2, 0x00,0xC0, 0x8,-12 }, // 511: dMM125; hxMM125; Helicopter
    { 0x20053EF,0x30210EF, 0x86,0xC0, 0xE,+12 }, // 512: dMM126; hxMM126; Applause
    { 0x2F0F00C,0x0E6F604, 0x00,0x00, 0xE,+0 }, // 513: dMM127; hxMM127; Gun Shot
    { 0x257F900,0x046FB00, 0x00,0x00, 0x0,+12 }, // 514: dMP35; hxMP35; Acoustic Bass Drum
    { 0x047FA00,0x006F900, 0x00,0x00, 0x6,+12 }, // 515: dMP36; hxMP36; Acoustic Bass Drum
    { 0x067FD02,0x078F703, 0x80,0x00, 0x6,+12 }, // 516: dMP37; hxMP37; Slide Stick
    { 0x214F70F,0x247F900, 0x05,0x00, 0xE,+12 }, // 517: dMP38; hxMP38; Acoustic Snare
    { 0x3FB88E1,0x2A8A6FF, 0x00,0x00, 0xF,+12 }, // 518: dMP39; hxMP39; Hand Clap
    { 0x0FFAA06,0x0FAF700, 0x00,0x00, 0xE,+12 }, // 519: dMP40; Electric Snare
    { 0x0F00000,0x0F00000, 0x3F,0x00, 0x0,+54 }, // 520: dMP40; Electric Snare
    { 0x06CF502,0x138F703, 0x00,0x00, 0x7,+12 }, // 521: dMP41; hxMP41; Low Floor Tom
    { 0x25E980C,0x306FB0F, 0x00,0x00, 0xF,+12 }, // 522: dMP42; hxMP42; Closed High-Hat
    { 0x078F502,0x137F700, 0x00,0x00, 0x7,+12 }, // 523: dMP43; hxMP43; High Floor Tom
    { 0x25E780C,0x32B8A0A, 0x00,0x80, 0xF,+12 }, // 524: dMP44; dMP69; hxMP44; hxMP69; Cabasa
    { 0x037F502,0x137F702, 0x00,0x00, 0x3,+12 }, // 525: dMP45; dMP47; dMP48; dMP50; hxMP45; hxMP47; hxMP48; hxMP50; High Tom
    { 0x201C700,0x233F90B, 0x45,0x00, 0xE,+12 }, // 526: dMP46; hxMP46; Open High Hat
    { 0x0E6C204,0x343E800, 0x10,0x00, 0xE,+12 }, // 527: dMP49; dMP57; hxMP49; hxMP57; Crash Cymbal 1
    { 0x212FD03,0x205FD02, 0x80,0x80, 0xA,+12 }, // 528: dMP51; dMP59; hxMP51; hxMP59; Ride Cymbal 1
    { 0x085E400,0x234D7C0, 0x80,0x80, 0xE,+12 }, // 529: dMP52; hxMP52; Chinses Cymbal
    { 0x0E6E204,0x144B801, 0x90,0x00, 0xE,+12 }, // 530: dMP53; hxMP53; Ride Bell
    { 0x2777602,0x3679801, 0x87,0x00, 0xF,+12 }, // 531: dMP54; hxMP54; Tambourine
    { 0x270F604,0x3A3C607, 0x81,0x00, 0xE,+12 }, // 532: dMP55; Splash Cymbal
    { 0x067FD00,0x098F601, 0x00,0x00, 0x6,+12 }, // 533: dMP56; hxMP56; Cowbell
    { 0x0B5F901,0x050D4BF, 0x07,0xC0, 0xB,+12 }, // 534: dMP58; hxMP58; Vibraslap
    { 0x256FB00,0x026FA00, 0x00,0x00, 0x4,+12 }, // 535: dMP60; dMP61; hxMP60; hxMP61; High Bongo
    { 0x256FB00,0x017F700, 0x80,0x00, 0x0,+12 }, // 536: dMP62; dMP63; dMP64; hxMP62; hxMP63; hxMP64; Low Conga
    { 0x056FB03,0x017F700, 0x81,0x00, 0x0,+12 }, // 537: dMP65; dMP66; hxMP65; hxMP66; High Timbale
    { 0x367FD01,0x098F601, 0x00,0x00, 0x8,+12 }, // 538: dMP67; dMP68; hxMP67; hxMP68; High Agogo
    { 0x2D65A00,0x0FFFFBF, 0x0E,0xC0, 0xA,+12 }, // 539: dMP70; hxMP70; Maracas
    { 0x1C7F900,0x0FFFF80, 0x07,0xC0, 0xA,+12 }, // 540: dMP71; dMP72; dMP73; dMP74; dMP79; hxMP71; hxMP72; hxMP73; hxMP74; hxMP79; Long Guiro
    { 0x1D1F813,0x078F512, 0x44,0x00, 0x6,+12 }, // 541: dMP75; dMP76; dMP77; hxMP75; hxMP76; hxMP77; Claves
    { 0x1DC5E01,0x0FFFFBF, 0x0B,0xC0, 0xA,+12 }, // 542: dMP78; hxMP78; Mute Cuica
    { 0x060F2C5,0x07AF4D4, 0x4F,0x80, 0x8,+12 }, // 543: dMP80; hxMP80; Mute Triangle
    { 0x160F285,0x0B7F294, 0x4F,0x80, 0x8,+12 }, // 544: dMP81; hxMP81; Open Triangle
    { 0x113F020,0x027E322, 0x8C,0x80, 0xA,+12 }, // 545: hxMM29; Overdriven Guitar
    { 0x125A020,0x136B220, 0x86,0x00, 0x6,+12 }, // 546: hxMM30; Distortion Guitar
    { 0x015C520,0x0A6D221, 0x28,0x00, 0xC,+12 }, // 547: hxMM32; Acoustic Bass
    { 0x1006010,0x0F68110, 0x1A,0x00, 0x8,+12 }, // 548: hxMM35; Fretless Bass
    { 0x2E7F030,0x047F131, 0x12,0x00, 0x0,+0 }, // 549: hxMM36; * Slap Bass 1
    { 0x1E7F510,0x2E7F610, 0x0D,0x00, 0xD,+12 }, // 550: hxMM36; * Slap Bass 1
    { 0x0465020,0x1569220, 0x96,0x80, 0xC,+12 }, // 551: hxMM58; Tuba
    { 0x075FC01,0x037F800, 0x00,0x00, 0x0,+12 }, // 552: hxMP40; Electric Snare
    { 0x175F701,0x336FC00, 0xC0,0x00, 0xC,+54 }, // 553: hxMP40; Electric Snare
    { 0x2709404,0x3A3C607, 0x81,0x00, 0xE,+12 }, // 554: hxMP55; Splash Cymbal
    { 0x132FA13,0x1F9F211, 0x80,0x0A, 0x8,+0 }, // 555: sGM6; Harpsichord
    { 0x0F2F409,0x0E2F211, 0x1B,0x80, 0x2,+0 }, // 556: sGM9; Glockenspiel
    { 0x0F3D403,0x0F3A340, 0x94,0x40, 0x6,+0 }, // 557: sGM14; Tubular Bells
    { 0x1058761,0x0058730, 0x80,0x03, 0x7,+0 }, // 558: sGM19; Church Organ
    { 0x174A423,0x0F8F271, 0x9D,0x80, 0xC,+0 }, // 559: sGM24; Acoustic Guitar1
    { 0x0007FF1,0x1167F21, 0x8D,0x00, 0x0,+0 }, // 560: sGM44; Tremulo Strings
    { 0x0759511,0x1F5C501, 0x0D,0x80, 0x0,+0 }, // 561: sGM45; Pizzicato String
    { 0x073F222,0x0F3F331, 0x97,0x80, 0x2,+0 }, // 562: sGM46; Orchestral Harp
    { 0x105F510,0x0C3F411, 0x41,0x00, 0x6,+0 }, // 563: sGM47; Timpany
    { 0x01096C1,0x1166221, 0x8B,0x00, 0x6,+0 }, // 564: sGM48; String Ensemble1
    { 0x01096C1,0x1153221, 0x8E,0x00, 0x6,+0 }, // 565: sGM49; String Ensemble2
    { 0x012C4A1,0x0065F61, 0x97,0x00, 0xE,+0 }, // 566: sGM50; Synth Strings 1
    { 0x010E4B1,0x0056A62, 0xCD,0x83, 0x0,+0 }, // 567: sGM52; Choir Aahs
    { 0x0F57591,0x144A440, 0x0D,0x00, 0xE,+0 }, // 568: sGM55; Orchestra Hit
    { 0x0256421,0x0088F21, 0x92,0x01, 0xC,+0 }, // 569: sGM56; Trumpet
    { 0x0167421,0x0078F21, 0x93,0x00, 0xC,+0 }, // 570: sGM57; Trombone
    { 0x0176421,0x0378261, 0x94,0x00, 0xC,+0 }, // 571: sGM58; Tuba
    { 0x0195361,0x0077F21, 0x94,0x04, 0xA,+0 }, // 572: sGM60; French Horn
    { 0x0187461,0x0088422, 0x8F,0x00, 0xA,+0 }, // 573: sGM61; Brass Section
    { 0x016A571,0x00A8F21, 0x4A,0x00, 0x8,+0 }, // 574: sGM68; Oboe
    { 0x00A8871,0x1198131, 0x4A,0x00, 0x0,+0 }, // 575: sGM70; Bassoon
    { 0x0219632,0x0187261, 0x4A,0x00, 0x4,+0 }, // 576: sGM71; Clarinet
    { 0x04A85E2,0x01A85E1, 0x59,0x00, 0x0,+0 }, // 577: sGM72; Piccolo
    { 0x02887E1,0x01975E1, 0x48,0x00, 0x0,+0 }, // 578: sGM73; Flute
    { 0x0451261,0x1045F21, 0x8E,0x84, 0x8,+0 }, // 579: sGM95; Pad 8 sweep
    { 0x106A510,0x004FA00, 0x86,0x03, 0x6,+0 }, // 580: sGM116; Taiko Drum
    { 0x202A50E,0x017A700, 0x09,0x00, 0xE,+0 }, // 581: sGM118; sGP38; sGP40; Acoustic Snare; Electric Snare; Synth Drum
    { 0x0001E0E,0x3FE1800, 0x00,0x00, 0xE,+0 }, // 582: sGM119; Reverse Cymbal
    { 0x0F6B710,0x005F011, 0x40,0x00, 0x6,+0 }, // 583: sGP35; sGP36; Ac Bass Drum; Bass Drum 1
    { 0x00BF506,0x008F602, 0x07,0x00, 0xA,+0 }, // 584: sGP37; Side Stick
    { 0x001FF0E,0x008FF0E, 0x00,0x00, 0xE,+0 }, // 585: sGP39; Hand Clap
    { 0x209F300,0x005F600, 0x06,0x00, 0x4,+0 }, // 586: sGP41; sGP43; sGP45; sGP47; sGP48; sGP50; High Floor Tom; High Tom; High-Mid Tom; Low Floor Tom; Low Tom; Low-Mid Tom
    { 0x006F60C,0x247FB12, 0x00,0x00, 0xE,+0 }, // 587: sGP42; Closed High Hat
    { 0x004F60C,0x244CB12, 0x00,0x05, 0xE,+0 }, // 588: sGP44; Pedal High Hat
    { 0x001F60C,0x242CB12, 0x00,0x00, 0xA,+0 }, // 589: sGP46; Open High Hat
    { 0x000F00E,0x3049F40, 0x00,0x00, 0xE,+0 }, // 590: sGP49; Crash Cymbal 1
    { 0x030F50E,0x0039F50, 0x00,0x04, 0xE,+0 }, // 591: sGP52; Chinese Cymbal
    { 0x204940E,0x0F78700, 0x02,0x0A, 0xA,+0 }, // 592: sGP54; Tambourine
    { 0x000F64E,0x2039F1E, 0x00,0x00, 0xE,+0 }, // 593: sGP55; Splash Cymbal
    { 0x000F60E,0x3029F50, 0x00,0x00, 0xE,+0 }, // 594: sGP57; Crash Cymbal 2
    { 0x100FF00,0x014FF10, 0x00,0x00, 0xC,+0 }, // 595: sGP58; Vibraslap
    { 0x04F760E,0x2187700, 0x40,0x03, 0xE,+0 }, // 596: sGP69; Cabasa
    { 0x1F4FC02,0x0F4F712, 0x00,0x05, 0x6,+0 }, // 597: sGP76; High Wood Block
    { 0x0B2F131,0x0AFF111, 0x8F,0x83, 0x8,+0 }, // 598: b46M0; b47M0; f20GM0; f31GM0; f36GM0; f48GM0; qGM0; AcouGrandPiano; gm000
    { 0x0B2F131,0x0D5C131, 0x19,0x01, 0x9,+0 }, // 599: b46M0; b47M0; f20GM0; f31GM0; f36GM0; f48GM0; qGM0; AcouGrandPiano; gm000
    { 0x0D2F111,0x0E6F211, 0x4C,0x83, 0xA,+0 }, // 600: b46M1; b47M1; f20GM1; f31GM1; f36GM1; f48GM1; qGM1; BrightAcouGrand; gm001
    { 0x0D5C111,0x0E6C231, 0x15,0x00, 0xB,+0 }, // 601: b46M1; b47M1; f20GM1; f31GM1; f36GM1; f48GM1; qGM1; BrightAcouGrand; gm001
    { 0x0D4F315,0x0E4B115, 0x5F,0x61, 0xE,+0 }, // 602: b46M2; b47M2; f20GM2; f31GM2; f36GM2; qGM2; ElecGrandPiano; gm002
    { 0x0E4B111,0x0B5B111, 0x5C,0x00, 0xE,+0 }, // 603: b46M2; b47M2; f20GM2; f31GM2; f36GM2; qGM2; ElecGrandPiano; gm002
    { 0x0D4F111,0x0E4C302, 0x89,0x5F, 0xD,+12 }, // 604: b46M3; b47M3; f20GM3; f31GM3; f36GM3; qGM3; Honky-tonkPiano; gm003
    { 0x035C100,0x0D5C111, 0x9B,0x00, 0xC,+0 }, // 605: f20GM3; f31GM3; f36GM3; qGM3; Honky-tonkPiano
    { 0x0E7F21C,0x0B8F201, 0x6F,0x80, 0xC,+0 }, // 606: b46M4; b47M4; f20GM4; f36GM4; f48GM4; f49GM4; qGM4; Rhodes Piano; gm004
    { 0x0E5B111,0x0B8F211, 0x9C,0x80, 0xD,+0 }, // 607: b46M4; b47M4; f20GM4; f31GM4; f36GM4; f48GM4; f49GM4; qGM4; Rhodes Piano; gm004
    { 0x0E7C21C,0x0B8F301, 0x3A,0x80, 0x0,+0 }, // 608: b46M5; b47M5; f20GM5; f31GM5; f36GM5; f48GM5; f49GM5; qGM5; Chorused Piano; gm005
    { 0x0F5B111,0x0D8F211, 0x1B,0x80, 0x1,+0 }, // 609: b46M5; b47M5; f20GM5; f31GM5; f36GM5; f48GM5; f49GM5; qGM5; Chorused Piano; gm005
    { 0x031F031,0x037F234, 0x90,0x9F, 0x8,+0 }, // 610: b46M6; b47M6; f20GM6; f31GM6; f36GM6; f48GM6; f49GM6; qGM6; Harpsichord; gm006
    { 0x451F324,0x497F211, 0x1C,0x00, 0x8,+0 }, // 611: b46M6; b47M6; f20GM6; f31GM6; f36GM6; f48GM6; f49GM6; qGM6; Harpsichord; gm006
    { 0x050F210,0x0F0E131, 0x60,0x5D, 0x4,+12 }, // 612: b46M7; b47M7; f20GM7; f31GM7; f36GM7; qGM7; Clavinet; gm007
    { 0x040B230,0x5E9F111, 0xA2,0x80, 0x4,+0 }, // 613: f20GM7; f31GM7; f36GM7; qGM7; Clavinet
    { 0x0E6CE02,0x0E6F401, 0x25,0x00, 0x0,+0 }, // 614: b46M8; b47M8; f20GM8; f36GM8; qGM8; Celesta; gm008
    { 0x0E6F507,0x0E5F341, 0xA1,0x00, 0x1,+0 }, // 615: b46M8; b47M8; f20GM8; f36GM8; qGM8; Celesta; gm008
    { 0x0E3F217,0x0E2C211, 0x54,0x06, 0xA,+0 }, // 616: b46M9; b47M9; f20GM9; f31GM9; f36GM9; qGM9; Glockenspiel; gm009
    { 0x0C3F219,0x0D2F291, 0x2B,0x07, 0xB,+0 }, // 617: b46M9; b47M9; f20GM9; f31GM9; f36GM9; qGM9; Glockenspiel; gm009
    { 0x004A61A,0x004F600, 0x27,0x0A, 0x3,+0 }, // 618: b46M10; b47M10; f20GM10; f31GM10; f36GM10; f49GM10; qGM10; Music box; gm010
    { 0x0790824,0x0E6E384, 0x9A,0x5B, 0xA,+12 }, // 619: b46M11; b47M11; f20GM11; f36GM11; f48GM11; f49GM11; qGM11; Vibraphone; gm011
    { 0x0E6F314,0x0E6F280, 0x62,0x00, 0xB,+0 }, // 620: f20GM11; f36GM11; f48GM11; f49GM11; qGM11; Vibraphone
    { 0x055F71C,0x0D88520, 0xA3,0x0D, 0x6,+0 }, // 621: b46M12; b47M12; f20GM12; f31GM12; f36GM12; f49GM12; qGM12; Marimba; gm012
    { 0x055F718,0x0D8E521, 0x23,0x00, 0x7,+0 }, // 622: b46M12; b47M12; f20GM12; f31GM12; f36GM12; f49GM12; qGM12; Marimba; gm012
    { 0x0D6F90A,0x0D6F784, 0x53,0x80, 0xA,+0 }, // 623: b46M13; b47M13; f20GM13; f31GM13; f36GM13; f49GM13; qGM13; Xylophone; gm013
    { 0x0A6F615,0x0E6F601, 0x91,0x00, 0xB,+0 }, // 624: b46M13; b47M13; f20GM13; f31GM13; f36GM13; f49GM13; qGM13; Xylophone; gm013
    { 0x0B3D441,0x0B4C280, 0x8A,0x13, 0x4,+0 }, // 625: b46M14; b47M14; f20GM14; f36GM14; f49GM14; qGM14; Tubular Bells; gm014
    { 0x082D345,0x0E3A381, 0x59,0x80, 0x5,+0 }, // 626: b46M14; b47M14; f20GM14; f36GM14; f49GM14; qGM14; Tubular Bells; gm014
    { 0x0F7E701,0x1557403, 0x84,0x49, 0xD,+0 }, // 627: b46M15; b47M15; f20GM15; f31GM15; f36GM15; f49GM15; qGM15; Dulcimer; gm015
    { 0x005B301,0x0F77601, 0x80,0x80, 0xD,+0 }, // 628: b46M15; b47M15; f20GM15; f31GM15; f36GM15; f49GM15; qGM15; Dulcimer; gm015
    { 0x02AA2A0,0x02AA522, 0x85,0x9E, 0x7,+0 }, // 629: b46M16; b47M16; f20GM16; f31GM16; f36GM16; f49GM16; qGM16; Hammond Organ; gm016
    { 0x02AA5A2,0x02AA128, 0x83,0x95, 0x7,+0 }, // 630: b46M16; b47M16; f20GM16; f31GM16; f36GM16; f49GM16; qGM16; Hammond Organ; gm016
    { 0x02A91A0,0x03AC821, 0x85,0x0B, 0x7,+0 }, // 631: b46M17; b47M17; f20GM17; f31GM17; f36GM17; f48GM17; f49GM17; qGM17; Percussive Organ; gm017
    { 0x038C620,0x057F621, 0x81,0x80, 0x7,+0 }, // 632: b46M17; b47M17; f20GM17; f31GM17; f36GM17; f49GM17; qGM17; Percussive Organ; gm017
    { 0x12AA6E3,0x00AAF61, 0x56,0x83, 0x8,-12 }, // 633: b46M18; b47M18; f20GM18; f31GM18; f36GM18; f49GM18; qGM18; Rock Organ; gm018
    { 0x00AAFE1,0x00AAF62, 0x91,0x83, 0x9,+0 }, // 634: f20GM18; f31GM18; f36GM18; qGM18; Rock Organ
    { 0x002B025,0x0057030, 0x5F,0x40, 0xC,+0 }, // 635: b46M19; b47M19; f20GM19; f31GM19; f36GM19; f48GM19; f49GM19; qGM19; Church Organ; gm019
    { 0x002C031,0x0056031, 0x46,0x80, 0xD,+0 }, // 636: b46M19; b47M19; f20GM19; f31GM19; f36GM19; f48GM19; f49GM19; qGM19; Church Organ; gm019
    { 0x015C821,0x0056F31, 0x93,0x00, 0xC,+0 }, // 637: b46M20; b47M20; f20GM20; f31GM20; f36GM20; f49GM20; qGM20; Reed Organ; gm020
    { 0x005CF31,0x0057F32, 0x16,0x87, 0xD,+0 }, // 638: b46M20; b47M20; f20GM20; f31GM20; f36GM20; f49GM20; qGM20; Reed Organ; gm020
    { 0x71A7223,0x02A7221, 0xAC,0x83, 0x0,+0 }, // 639: b46M21; b47M21; f20GM21; f31GM21; f36GM21; f49GM21; qGM21; Accordion; gm021
    { 0x41A6223,0x02A62A1, 0x22,0x00, 0x1,+0 }, // 640: b46M21; b47M21; f20GM21; f31GM21; f36GM21; f49GM21; qGM21; Accordion; gm021
    { 0x006FF25,0x005FF23, 0xA1,0x2F, 0xA,+0 }, // 641: b46M22; b47M22; f20GM22; f31GM22; f36GM22; f48GM22; f49GM22; qGM22; Harmonica; gm022
    { 0x405FFA1,0x0096F22, 0x1F,0x80, 0xA,+0 }, // 642: b46M22; b47M22; f20GM22; f31GM22; f36GM22; f48GM22; f49GM22; qGM22; Harmonica; gm022
    { 0x11A6223,0x02A7221, 0x19,0x80, 0xC,+0 }, // 643: b46M23; b47M23; f20GM23; f31GM23; f36GM23; f48GM23; f49GM23; qGM23; Tango Accordion; gm023
    { 0x41A6223,0x02A7222, 0x1E,0x83, 0xD,+0 }, // 644: b46M23; b47M23; f20GM23; f31GM23; f36GM23; f48GM23; f49GM23; qGM23; Tango Accordion; gm023
    { 0x074F302,0x0B8F341, 0x9C,0x80, 0xA,+0 }, // 645: b46M24; b47M24; f20GM24; f31GM24; f36GM24; f48GM24; qGM24; Acoustic Guitar1; gm024
    { 0x274D302,0x0B8D382, 0xA5,0x40, 0xB,+0 }, // 646: b46M24; b47M24; f20GM24; f31GM24; f36GM24; f48GM24; qGM24; Acoustic Guitar1; gm024
    { 0x2F6F234,0x0F7F231, 0x5B,0x9E, 0xC,+0 }, // 647: b46M25; b47M25; f20GM25; f31GM25; f36GM25; f48GM25; f49GM25; qGM25; Acoustic Guitar2; gm025
    { 0x0F7F223,0x0E7F111, 0xAB,0x00, 0xC,+0 }, // 648: b46M25; b47M25; f20GM25; f31GM25; f36GM25; f48GM25; f49GM25; qGM25; Acoustic Guitar2; gm025
    { 0x0FAF322,0x0FAF223, 0x53,0x66, 0xA,+0 }, // 649: b46M26; b47M26; f20GM26; f31GM26; f36GM26; f48GM26; f49GM26; qGM26; Electric Guitar1; gm026
    { 0x0FAC221,0x0F7C221, 0xA7,0x00, 0xA,+0 }, // 650: b46M26; b47M26; f20GM26; f31GM26; f36GM26; f48GM26; f49GM26; qGM26; Electric Guitar1; gm026
    { 0x022FA02,0x0F3F301, 0x4C,0x97, 0x8,+0 }, // 651: b46M27; b47M27; f20GM27; f31GM27; f36GM27; qGM27; Electric Guitar2; gm027
    { 0x1F3C204,0x0F7C111, 0x9D,0x00, 0x8,+0 }, // 652: b46M27; b47M27; f20GM27; f31GM27; f36GM27; qGM27; Electric Guitar2; gm027
    { 0x0AFC711,0x0F8F501, 0x87,0x00, 0x8,+0 }, // 653: b46M28; b47M28; f20GM28; f31GM28; f36GM28; f48GM28; qGM28; Electric Guitar3; gm028
    { 0x098C301,0x0F8C302, 0x18,0x00, 0x9,+0 }, // 654: b46M28; b47M28; f20GM28; f31GM28; f36GM28; f48GM28; qGM28; Electric Guitar3; gm028
    { 0x4F2B913,0x0119102, 0x0D,0x1A, 0xA,+0 }, // 655: b46M29; b47M29; f20GM29; f31GM29; f36GM29; f49GM1; qGM29; BrightAcouGrand; Overdrive Guitar; gm029
    { 0x14A9221,0x02A9122, 0x99,0x00, 0xA,+0 }, // 656: b46M29; b47M29; f20GM29; f31GM29; f36GM29; f48GM29; f49GM1; qGM29; BrightAcouGrand; Overdrive Guitar; gm029
    { 0x242F823,0x2FA9122, 0x96,0x1A, 0x0,+0 }, // 657: b46M30; b47M30; f20GM30; f31GM30; f36GM30; qGM30; Distorton Guitar; gm030
    { 0x0BA9221,0x04A9122, 0x99,0x00, 0x0,+0 }, // 658: b46M30; b47M30; f20GM30; f31GM30; f36GM30; qGM30; Distorton Guitar; gm030
    { 0x04F2009,0x0F8D104, 0xA1,0x80, 0x8,+0 }, // 659: b46M31; b47M31; f20GM31; f31GM31; f36GM31; qGM31; Guitar Harmonics; gm031
    { 0x2F8F802,0x0F8F602, 0x87,0x00, 0x9,+0 }, // 660: b46M31; b47M31; f20GM31; f31GM31; f36GM31; qGM31; Guitar Harmonics; gm031
    { 0x015A701,0x0C8A301, 0x4D,0x00, 0x2,+0 }, // 661: b46M32; b47M32; f20GM32; f31GM32; f36GM32; qGM32; Acoustic Bass; gm032
    { 0x0317101,0x0C87301, 0x93,0x00, 0x3,+0 }, // 662: b46M32; b47M32; f20GM32; f31GM32; f36GM32; qGM32; Acoustic Bass; gm032
    { 0x0E5F111,0x0E5F312, 0xA8,0x57, 0x4,+0 }, // 663: b46M33; b47M33; f20GM33; f31GM33; f36GM33; f49GM39; qGM33; Electric Bass 1; Synth Bass 2; gm033
    { 0x0E5E111,0x0E6E111, 0x97,0x00, 0x4,+0 }, // 664: b46M33; b47M33; f20GM33; f31GM33; f36GM33; f49GM39; qGM33; Electric Bass 1; Synth Bass 2; gm033
    { 0x0C7F001,0x027F101, 0xB3,0x16, 0x6,+0 }, // 665: b46M34; b47M34; f20GM34; f31GM34; f36GM34; qGM34; Electric Bass 2; gm034
    { 0x027F101,0x028F101, 0x16,0x00, 0x6,+0 }, // 666: b46M34; b47M34; f20GM34; f31GM34; f36GM34; qGM34; Electric Bass 2; gm034
    { 0x0487131,0x0487131, 0x19,0x00, 0xD,+0 }, // 667: b46M35; b47M35; f20GM35; f31GM35; f36GM35; f49GM35; qGM35; Fretless Bass; gm035
    { 0x0DAF904,0x0DFF701, 0x0B,0x80, 0x9,+0 }, // 668: b46M36; b47M36; f20GM36; f31GM36; f36GM36; f49GM36; qGM36; Slap Bass 1; gm036
    { 0x09AA101,0x0DFF221, 0x89,0x40, 0x6,+0 }, // 669: b46M37; b47M37; f20GM37; f31GM37; f36GM37; qGM37; Slap Bass 2; gm037
    { 0x0DAF904,0x0DFF701, 0x0B,0x80, 0x7,+0 }, // 670: b46M37; b47M37; f20GM37; f31GM37; f36GM37; f49GM37; qGM37; Slap Bass 2; gm037
    { 0x0C8F621,0x0C8F101, 0x1C,0x1F, 0xA,+0 }, // 671: b46M38; b47M38; f20GM38; f31GM38; f36GM38; qGM38; Synth Bass 1; gm038
    { 0x0C8F101,0x0C8F201, 0xD8,0x00, 0xA,+0 }, // 672: b46M38; b47M38; f20GM38; f31GM38; f36GM38; qGM38; Synth Bass 1; gm038
    { 0x1C8F621,0x0C8F101, 0x1C,0x1F, 0xA,+0 }, // 673: b46M39; b47M39; f20GM39; f31GM39; f36GM39; qGM39; Synth Bass 2; gm039
    { 0x0425401,0x0C8F201, 0x12,0x00, 0xA,+0 }, // 674: b46M39; b47M39; f20GM39; f31GM39; f36GM39; qGM39; Synth Bass 2; gm039
    { 0x1038D12,0x0866503, 0x95,0x8B, 0x9,+0 }, // 675: b46M40; b46M41; b47M40; b47M41; f20GM40; f20GM41; f31GM40; f31GM41; f36GM40; f36GM41; f48GM40; f49GM40; f49GM41; qGM40; qGM41; Viola; Violin; gm040; gm041
    { 0x113DD31,0x0265621, 0x17,0x00, 0x8,+0 }, // 676: b46M41; b47M41; f20GM41; f31GM41; f36GM41; f49GM41; qGM41; Viola; gm041
    { 0x513DD31,0x0265621, 0x95,0x00, 0x8,+0 }, // 677: b46M42; b47M42; f20GM42; f31GM42; f36GM42; f48GM45; f49GM42; qGM42; Cello; Pizzicato String; gm042
    { 0x1038D13,0x0866605, 0x95,0x8C, 0x9,+0 }, // 678: b46M42; b47M42; f20GM42; f31GM42; f36GM42; f49GM42; qGM42; Cello; gm042
    { 0x243CC70,0x21774A0, 0x92,0x03, 0xE,+0 }, // 679: b46M43; b47M43; f20GM43; f31GM43; f36GM43; f48GM43; f49GM43; qGM43; Contrabass; gm043
    { 0x007BF21,0x1076F21, 0x95,0x00, 0xF,+0 }, // 680: b46M43; b47M43; f20GM43; f31GM43; f36GM43; f48GM43; f49GM43; qGM43; Contrabass; gm043
    { 0x515C261,0x0056FA1, 0x97,0x00, 0x6,+0 }, // 681: b46M44; b47M44; f20GM44; f31GM44; f36GM44; f49GM44; qGM44; Tremulo Strings; gm044
    { 0x08FB563,0x08FB5A5, 0x13,0x94, 0x7,+0 }, // 682: b46M44; b47M44; f20GM44; f31GM44; f36GM44; f49GM44; qGM44; Tremulo Strings; gm044
    { 0x0848523,0x0748212, 0xA7,0xA4, 0xE,+0 }, // 683: b46M45; b46M46; b47M45; b47M46; f20GM45; f20GM46; f36GM45; f36GM46; f48GM46; f49GM45; f49GM46; qGM45; qGM46; Orchestral Harp; Pizzicato String; gm045; gm046
    { 0x0748202,0x0358511, 0x27,0x00, 0xE,+0 }, // 684: b46M45; b47M45; f20GM45; f36GM45; f49GM45; qGM45; Pizzicato String; gm045
    { 0x0748202,0x0338411, 0x27,0x00, 0xE,+0 }, // 685: b46M46; b47M46; f20GM46; f36GM46; f48GM46; f49GM46; qGM46; Orchestral Harp; gm046
    { 0x005F511,0x0C3F212, 0x01,0x1E, 0x3,+0 }, // 686: b46M47; b47M47; f20GM47; f36GM47; qGM47; Timpany; gm047
    { 0x2036130,0x21764A0, 0x98,0x03, 0xE,+0 }, // 687: b46M48; b47M48; f20GM48; f36GM48; f48GM48; f49GM48; qGM48; String Ensemble1; gm048
    { 0x1176561,0x0176521, 0x92,0x00, 0xF,+0 }, // 688: b46M48; b47M48; f20GM48; f36GM48; f49GM48; qGM48; String Ensemble1; gm048
    { 0x2234130,0x2174460, 0x98,0x01, 0xE,+0 }, // 689: b46M49; b47M49; f20GM49; f36GM49; f49GM49; qGM49; String Ensemble2; gm049
    { 0x1037FA1,0x1073F21, 0x98,0x00, 0xF,+0 }, // 690: b46M49; b47M49; f20GM49; f36GM49; f49GM49; qGM49; String Ensemble2; gm049
    { 0x012C121,0x0054F61, 0x1A,0x00, 0xC,+0 }, // 691: b46M50; b47M50; f20GM50; f36GM50; f48GM50; f49GM50; qGM50; Synth Strings 1; gm050
    { 0x012C1A1,0x0054F21, 0x93,0x00, 0xD,+0 }, // 692: b46M50; b47M50; f20GM50; f36GM50; f48GM50; qGM50; Synth Strings 1; gm050
    { 0x022C122,0x0054F22, 0x0B,0x1C, 0xD,+0 }, // 693: b46M51; b47M51; f20GM51; f31GM51; f36GM51; f49GM51; qGM51; SynthStrings 2; gm051
    { 0x0F5A006,0x035A3E4, 0x03,0x23, 0xE,+0 }, // 694: b46M52; b47M52; f20GM52; f31GM52; f36GM52; f49GM52; qGM52; Choir Aahs; gm052
    { 0x0077FA1,0x0077F61, 0x51,0x00, 0xF,+0 }, // 695: b46M52; b47M52; f20GM52; f31GM52; f36GM52; f49GM52; qGM52; Choir Aahs; gm052
    { 0x0578402,0x074A7E4, 0x05,0x16, 0xE,+0 }, // 696: b46M53; b47M53; f20GM53; f31GM53; f36GM53; f49GM53; qGM53; Voice Oohs; gm053
    { 0x03974A1,0x0677161, 0x90,0x00, 0xF,+0 }, // 697: b46M53; b47M53; f20GM53; f31GM53; f36GM53; f49GM53; qGM53; Voice Oohs; gm053
    { 0x054990A,0x0639707, 0x65,0x60, 0x8,+0 }, // 698: b46M54; b47M54; f20GM54; f31GM54; f36GM54; f48GM54; f49GM54; qGM54; Synth Voice; gm054
    { 0x1045FA1,0x0066F61, 0x59,0x00, 0x8,+0 }, // 699: b46M54; b47M54; f20GM54; f31GM54; f36GM54; f48GM54; f49GM54; qGM54; Synth Voice; gm054
    { 0x2686500,0x613C500, 0x00,0x00, 0xB,+0 }, // 700: b46M55; b47M55; f20GM55; f31GM55; f36GM55; f49GM55; qGM55; Orchestra Hit; gm055
    { 0x606C800,0x3077400, 0x00,0x00, 0xB,+0 }, // 701: b46M55; b47M55; f20GM55; f31GM55; f36GM55; f49GM55; qGM55; Orchestra Hit; gm055
    { 0x0178421,0x008AF61, 0x15,0x0B, 0xD,+0 }, // 702: b46M56; b47M56; f20GM56; f31GM56; f36GM56; f49GM56; qGM56; Trumpet; gm056
    { 0x0178521,0x0097F21, 0x94,0x05, 0xC,+0 }, // 703: b46M57; b47M57; f20GM57; f31GM57; f36GM57; f49GM57; qGM57; Trombone; gm057
    { 0x0178421,0x008AF61, 0x15,0x0D, 0xD,+0 }, // 704: b46M57; b47M57; f20GM57; f31GM57; f36GM57; f49GM57; qGM57; Trombone; gm057
    { 0x0157620,0x0378261, 0x94,0x00, 0xC,+12 }, // 705: b46M58; b47M58; f20GM58; f31GM58; f36GM58; f49GM58; qGM58; Tuba; gm058
    { 0x02661B1,0x0266171, 0xD3,0x80, 0xD,+0 }, // 706: f20GM58; f31GM58; f36GM58; f49GM58; qGM58; Tuba
    { 0x1277131,0x0499161, 0x15,0x83, 0xC,+0 }, // 707: b46M59; b47M59; f20GM59; f31GM59; f36GM59; f49GM59; qGM59; Muted Trumpet; gm059
    { 0x0277DB1,0x0297A21, 0x10,0x08, 0xD,+0 }, // 708: b46M59; b47M59; f20GM59; f31GM59; f36GM59; f49GM59; qGM59; Muted Trumpet; gm059
    { 0x00A6321,0x00B7F21, 0x9F,0x00, 0xE,+0 }, // 709: b46M60; b47M60; f20GM60; f36GM60; f49GM60; qGM60; French Horn; gm060
    { 0x00A65A1,0x00B7F61, 0xA2,0x00, 0xF,+0 }, // 710: b46M60; b47M60; f20GM60; f36GM60; f49GM60; qGM60; French Horn; gm060
    { 0x0257221,0x00A7F21, 0x16,0x05, 0xC,+0 }, // 711: b46M61; b47M61; f20GM61; f36GM61; f49GM61; qGM61; Brass Section; gm061
    { 0x0357A21,0x03A7A21, 0x1D,0x09, 0xD,+0 }, // 712: b46M61; b47M61; f20GM61; f36GM61; f49GM61; qGM61; Brass Section; gm061
    { 0x035C221,0x00ACF61, 0x16,0x09, 0xE,+0 }, // 713: b46M62; b47M62; f20GM62; f31GM62; f36GM62; f49GM62; qGM62; Synth Brass 1; gm062
    { 0x04574A1,0x0087F21, 0x8A,0x00, 0xF,+0 }, // 714: b46M62; b47M62; f20GM62; f31GM62; f36GM62; f49GM62; qGM62; Synth Brass 1; gm062
    { 0x01A52A1,0x01B8F61, 0x97,0x00, 0xC,+0 }, // 715: b46M63; b47M63; f20GM63; f36GM63; f49GM63; qGM63; Synth Brass 2; gm063
    { 0x01A7521,0x01B8F21, 0xA1,0x00, 0xD,+0 }, // 716: b46M63; b47M63; f20GM63; f36GM63; f49GM63; qGM63; Synth Brass 2; gm063
    { 0x20F9331,0x00F72A1, 0x96,0x00, 0x8,+0 }, // 717: b46M64; b47M64; f20GM64; f31GM64; f36GM64; f49GM64; qGM64; Soprano Sax; gm064
    { 0x0078521,0x1278431, 0x96,0x00, 0x9,+0 }, // 718: b46M64; b47M64; f20GM64; f31GM64; f36GM64; f49GM64; qGM64; Soprano Sax; gm064
    { 0x1039331,0x00972A1, 0x8E,0x00, 0x8,+0 }, // 719: b46M65; b47M65; f17GM65; f20GM65; f31GM65; f35GM65; f36GM65; f49GM65; mGM65; qGM65; Alto Sax; gm065
    { 0x006C524,0x1276431, 0xA1,0x00, 0x9,+0 }, // 720: b46M65; b47M65; f20GM65; f31GM65; f36GM65; f49GM65; qGM65; Alto Sax; gm065
    { 0x10693B1,0x0067271, 0x8E,0x00, 0xA,+0 }, // 721: b46M66; b47M66; f20GM66; f31GM66; f36GM66; f49GM66; qGM66; Tenor Sax; gm066
    { 0x0088521,0x02884B1, 0x5D,0x00, 0xB,+0 }, // 722: b46M66; b47M66; f20GM66; f31GM66; f36GM66; f49GM66; qGM66; Tenor Sax; gm066
    { 0x10F9331,0x00F7272, 0x93,0x00, 0xC,+0 }, // 723: b46M67; b47M67; f20GM67; f31GM67; f36GM67; f48GM67; f49GM67; qGM67; Baritone Sax; gm067
    { 0x0068522,0x01684B1, 0x61,0x00, 0xD,+0 }, // 724: b46M67; b47M67; f20GM67; f31GM67; f36GM67; f48GM67; f49GM67; qGM67; Baritone Sax; gm067
    { 0x02AA961,0x036A823, 0xA3,0x52, 0x8,+0 }, // 725: b46M68; b47M68; f20GM68; f36GM68; f49GM68; qGM68; Oboe; gm068
    { 0x016AAA1,0x00A8F21, 0x94,0x80, 0x8,+0 }, // 726: b46M68; b47M68; f20GM68; f36GM68; f49GM68; qGM68; Oboe; gm068
    { 0x0297721,0x1267A33, 0x21,0x55, 0x2,+0 }, // 727: b46M69; b47M69; f20GM69; f31GM69; f36GM69; f49GM69; qGM69; English Horn; gm069
    { 0x0167AA1,0x0197A22, 0x93,0x00, 0x2,+0 }, // 728: b46M69; b47M69; f20GM69; f31GM69; f36GM69; f49GM69; qGM69; English Horn; gm069
    { 0x1077B21,0x0007F22, 0x2B,0x57, 0xA,+0 }, // 729: b46M70; b47M70; f20GM70; f31GM70; f36GM70; f49GM70; qGM70; Bassoon; gm070
    { 0x0197531,0x0196172, 0x51,0x00, 0xA,+0 }, // 730: b46M70; b47M70; f20GM70; f31GM70; f36GM70; f49GM70; qGM70; Bassoon; gm070
    { 0x0219B32,0x0177221, 0x90,0x00, 0x8,+0 }, // 731: b46M71; b47M71; f20GM71; f31GM71; f36GM71; f49GM71; qGM71; Clarinet; gm071
    { 0x0219B32,0x0177221, 0x90,0x13, 0x9,+0 }, // 732: b46M71; b47M71; f20GM71; f31GM71; f36GM71; f49GM71; qGM71; Clarinet; gm071
    { 0x011DA25,0x068A6E3, 0x00,0x2B, 0xC,+0 }, // 733: b46M72; b46M73; b47M72; b47M73; f20GM72; f20GM73; f31GM72; f31GM73; f36GM72; f36GM73; f49GM72; f49GM73; qGM72; qGM73; Flute; Piccolo; gm072; gm073
    { 0x05F85E1,0x01A65E1, 0x1F,0x00, 0xD,+0 }, // 734: b46M72; b47M72; f20GM72; f31GM72; f36GM72; f49GM72; qGM72; Piccolo; gm072
    { 0x05F88E1,0x01A65E1, 0x46,0x00, 0xD,+0 }, // 735: b46M73; b47M73; f20GM73; f31GM73; f36GM73; f49GM73; qGM73; Flute; gm073
    { 0x029C9A4,0x0086F21, 0xA2,0x80, 0xC,+0 }, // 736: b46M74; b47M74; f20GM74; f36GM74; f48GM74; f49GM74; qGM74; Recorder; gm074
    { 0x015CAA2,0x0086F21, 0xAA,0x00, 0xD,+0 }, // 737: b46M74; b47M74; f20GM74; f36GM74; f48GM74; f49GM74; qGM74; Recorder; gm074
    { 0x011DA25,0x068A623, 0x00,0x1E, 0xC,+0 }, // 738: b46M75; b47M75; f20GM75; f31GM75; f36GM75; qGM75; Pan Flute; gm075
    { 0x0588821,0x01A6521, 0x8C,0x00, 0xD,+0 }, // 739: b46M75; b47M75; f20GM75; f31GM75; f36GM75; qGM75; Pan Flute; gm075
    { 0x0C676A1,0x0868726, 0x0D,0x59, 0xF,+0 }, // 740: b46M76; b47M76; f20GM76; f31GM76; f36GM76; f48GM76; f49GM76; qGM76; Bottle Blow; gm076
    { 0x0566622,0x02665A1, 0x56,0x00, 0xE,+0 }, // 741: b46M76; b47M76; f20GM76; f31GM76; f36GM76; f48GM76; f49GM76; qGM76; Bottle Blow; gm076
    { 0x0019F26,0x0487664, 0x00,0x25, 0xE,+0 }, // 742: b46M77; b47M77; f20GM77; f31GM77; f36GM77; f49GM77; qGM77; Shakuhachi; gm077
    { 0x0465622,0x03645A1, 0xCB,0x00, 0xF,+0 }, // 743: b46M77; b47M77; f20GM77; f31GM77; f36GM77; f49GM77; qGM77; Shakuhachi; gm077
    { 0x11467E1,0x0175461, 0x67,0x00, 0xC,+0 }, // 744: b46M78; b47M78; f20GM78; f31GM78; f36GM78; f48GM78; f49GM78; qGM78; Whistle; gm078
    { 0x1146721,0x0164421, 0x6D,0x00, 0xD,+0 }, // 745: b46M78; b47M78; f20GM78; f31GM78; f36GM78; f48GM78; f49GM78; qGM78; Whistle; gm078
    { 0x001DF26,0x03876E4, 0x00,0x2B, 0xC,+0 }, // 746: b46M79; b47M79; f20GM79; f31GM79; f36GM79; f48GM79; qGM79; Ocarina; gm079
    { 0x0369522,0x00776E1, 0xD8,0x00, 0xD,+0 }, // 747: b46M79; b47M79; f20GM79; f31GM79; f36GM79; f48GM79; qGM79; Ocarina; gm079
    { 0x00FFF21,0x00FFF21, 0x35,0xB7, 0x4,+0 }, // 748: b46M80; b47M80; f20GM80; f31GM80; f36GM80; f49GM80; qGM80; Lead 1 squareea; gm080
    { 0x00FFF21,0x60FFF21, 0xB9,0x80, 0x4,+0 }, // 749: b46M80; b47M80; f20GM80; f31GM80; f36GM80; f49GM80; qGM80; Lead 1 squareea; gm080
    { 0x00FFF21,0x00FFF21, 0x36,0x1B, 0xA,+0 }, // 750: b46M81; b47M81; f20GM81; f31GM81; f36GM81; f49GM79; f49GM81; qGM81; Lead 2 sawtooth; Ocarina; gm081
    { 0x00FFF21,0x409CF61, 0x1D,0x00, 0xA,+0 }, // 751: b46M81; b47M81; f20GM81; f31GM81; f36GM81; f49GM81; qGM81; Lead 2 sawtooth; gm081
    { 0x087C4A3,0x076C626, 0x00,0x57, 0xE,+0 }, // 752: b46M82; b47M82; f20GM82; f31GM82; f36GM82; f49GM82; qGM82; Lead 3 calliope; gm082
    { 0x0558622,0x0186421, 0x46,0x80, 0xF,+0 }, // 753: b46M82; b47M82; f20GM82; f31GM82; f36GM82; f49GM82; qGM82; Lead 3 calliope; gm082
    { 0x04AA321,0x00A8621, 0x48,0x00, 0x8,+0 }, // 754: b46M83; b47M83; f20GM83; f31GM83; f36GM83; f48GM83; f49GM83; qGM83; Lead 4 chiff; gm083
    { 0x0126621,0x00A9621, 0x45,0x00, 0x9,+0 }, // 755: b46M83; b47M83; f20GM83; f31GM83; f36GM83; f48GM83; f49GM83; qGM83; Lead 4 chiff; gm083
    { 0x4F2B912,0x0119101, 0x0D,0x1A, 0xA,+0 }, // 756: b46M84; b47M84; f20GM84; f31GM84; f36GM84; f48GM84; f49GM84; qGM84; Lead 5 charang; gm084
    { 0x12A9221,0x02A9122, 0x99,0x00, 0xA,+0 }, // 757: b46M84; b47M84; f20GM84; f31GM84; f36GM84; f48GM84; f49GM84; qGM84; Lead 5 charang; gm084
    { 0x0157D61,0x01572B1, 0x40,0xA3, 0xE,+0 }, // 758: b46M85; b47M85; f20GM85; f31GM85; f36GM85; qGM85; Lead 6 voice; gm085
    { 0x005DFA2,0x0077F61, 0x5D,0x40, 0xF,+0 }, // 759: b46M85; b47M85; f20GM85; f31GM85; f36GM85; qGM85; Lead 6 voice; gm085
    { 0x001FF20,0x4068F61, 0x36,0x00, 0x8,+0 }, // 760: b46M86; b47M86; f20GM86; f31GM86; f36GM86; qGM86; Lead 7 fifths; gm086
    { 0x00FFF21,0x4078F61, 0x27,0x00, 0x9,+0 }, // 761: b46M86; b47M86; f20GM86; f31GM86; f36GM86; qGM86; Lead 7 fifths; gm086
    { 0x109F121,0x109F121, 0x1D,0x80, 0xB,+0 }, // 762: b46M87; b47M87; f20GM87; f31GM87; f36GM87; qGM87; Lead 8 brass; gm087
    { 0x1035317,0x004F608, 0x1A,0x0D, 0x2,+0 }, // 763: b46M88; b47M88; f20GM88; f36GM88; f48GM88; qGM88; Pad 1 new age; gm088
    { 0x03241A1,0x0156161, 0x9D,0x00, 0x3,+0 }, // 764: b46M88; b47M88; f20GM88; f36GM88; f48GM88; qGM88; Pad 1 new age; gm088
    { 0x031A181,0x0032571, 0xA1,0x00, 0xB,+0 }, // 765: b46M89; b47M89; f20GM89; f31GM89; f36GM89; f49GM89; qGM89; Pad 2 warm; gm089
    { 0x0141161,0x0165561, 0x17,0x00, 0xC,+0 }, // 766: b46M90; b47M90; f20GM90; f31GM63; f31GM90; f36GM90; f49GM90; qGM90; Pad 3 polysynth; Synth Brass 2; gm090
    { 0x445C361,0x025C361, 0x14,0x00, 0xD,+0 }, // 767: b46M90; b47M90; f20GM90; f31GM60; f31GM63; f31GM90; f36GM90; f49GM90; qGM90; French Horn; Pad 3 polysynth; Synth Brass 2; gm090
    { 0x021542A,0x0136A27, 0x80,0xA6, 0xE,+0 }, // 768: b46M91; b47M91; f20GM91; f31GM91; f36GM91; f48GM91; f49GM91; qGM91; Pad 4 choir; gm091
    { 0x0015431,0x0036A72, 0x5D,0x00, 0xF,+0 }, // 769: b46M91; b47M91; f20GM91; f31GM91; f36GM91; f48GM91; f49GM91; qGM91; Pad 4 choir; gm091
    { 0x0332121,0x0454222, 0x97,0x03, 0x8,+0 }, // 770: b46M92; b47M92; f20GM92; f31GM92; f36GM92; f49GM92; qGM92; Pad 5 bowedpad; gm092
    { 0x0D421A1,0x0D54221, 0x99,0x03, 0x9,+0 }, // 771: b46M92; b47M92; f20GM92; f31GM92; f36GM92; f49GM92; qGM92; Pad 5 bowedpad; gm092
    { 0x0336121,0x0354261, 0x8D,0x03, 0xA,+0 }, // 772: b46M93; b47M93; f20GM93; f31GM93; f36GM93; f49GM93; qGM93; Pad 6 metallic; gm093
    { 0x177A1A1,0x1473121, 0x1C,0x00, 0xB,+0 }, // 773: b46M93; b47M93; f20GM93; f31GM93; f36GM93; f49GM93; qGM93; Pad 6 metallic; gm093
    { 0x0331121,0x0354261, 0x89,0x03, 0xA,+0 }, // 774: b46M94; b47M94; f20GM94; f31GM94; f36GM94; f49GM94; qGM94; Pad 7 halo; gm094
    { 0x0E42121,0x0D54261, 0x8C,0x03, 0xB,+0 }, // 775: b46M94; b47M94; f20GM94; f31GM94; f36GM94; f49GM94; qGM94; Pad 7 halo; gm094
    { 0x1471121,0x007CF21, 0x15,0x00, 0x0,+0 }, // 776: b46M95; b47M95; f20GM95; f31GM95; f36GM95; f48GM95; f49GM95; qGM95; Pad 8 sweep; gm095
    { 0x0E41121,0x0D55261, 0x8C,0x00, 0x1,+0 }, // 777: b46M95; b47M95; f20GM95; f31GM95; f36GM95; f48GM95; qGM95; Pad 8 sweep; gm095
    { 0x58AFE0F,0x006FB04, 0x83,0x85, 0xC,+0 }, // 778: b46M96; b47M96; f20GM96; f31GM96; f36GM96; f48GM96; f49GM96; qGM96; FX 1 rain; gm096
    { 0x003A821,0x004A722, 0x99,0x00, 0xD,+0 }, // 779: b46M96; b47M96; f20GM96; f31GM96; f36GM96; f49GM96; qGM96; FX 1 rain; gm096
    { 0x2322121,0x0133220, 0x8C,0x97, 0x6,+0 }, // 780: b46M97; b47M97; f20GM97; f31GM97; f36GM97; f49GM97; qGM97; FX 2 soundtrack; gm097
    { 0x1031121,0x0133121, 0x0E,0x00, 0x7,+0 }, // 781: b46M97; b47M97; f20GM97; f31GM97; f36GM97; f49GM97; qGM97; FX 2 soundtrack; gm097
    { 0x0937501,0x0B4C502, 0x61,0x80, 0x8,+0 }, // 782: b46M98; b47M98; f20GM98; f31GM98; f36GM98; f48GM98; f49GM98; qGM98; FX 3 crystal; gm098
    { 0x0957406,0x072A501, 0x5B,0x00, 0x9,+0 }, // 783: b46M98; b47M98; f20GM98; f31GM98; f36GM98; f48GM98; f49GM98; qGM98; FX 3 crystal; gm098
    { 0x056B222,0x056F261, 0x92,0x8A, 0xC,+0 }, // 784: b46M99; b47M99; f20GM99; f31GM99; f36GM99; qGM99; FX 4 atmosphere; gm099
    { 0x2343121,0x00532A1, 0x9D,0x80, 0xD,+0 }, // 785: b46M99; b47M99; f20GM99; f31GM99; f36GM99; qGM99; FX 4 atmosphere; gm099
    { 0x088A324,0x087A322, 0x40,0x5B, 0xE,+0 }, // 786: b46M100; b47M100; f20GM100; f31GM100; f36GM100; f48GM100; f49GM100; qGM100; FX 5 brightness; gm100
    { 0x151F101,0x0F5F241, 0x13,0x00, 0xF,+0 }, // 787: b46M100; b47M100; f20GM100; f31GM100; f36GM100; f48GM100; f49GM100; qGM100; FX 5 brightness; gm100
    { 0x04211A1,0x0731161, 0x10,0x92, 0xA,+0 }, // 788: b46M101; b47M101; f20GM101; f31GM101; f36GM101; f49GM101; qGM101; FX 6 goblins; gm101
    { 0x0211161,0x0031DA1, 0x98,0x80, 0xB,+0 }, // 789: b46M101; b47M101; f20GM101; f31GM101; f36GM101; f49GM101; qGM101; FX 6 goblins; gm101
    { 0x0167D62,0x01672A2, 0x57,0x80, 0x4,+0 }, // 790: b46M102; b47M102; f20GM102; f31GM102; f36GM102; f49GM102; qGM102; FX 7 echoes; gm102
    { 0x0069F61,0x0049FA1, 0x5B,0x00, 0x5,+0 }, // 791: b46M102; b47M102; f20GM102; f31GM102; f36GM102; f49GM102; qGM102; FX 7 echoes; gm102
    { 0x024A238,0x024F231, 0x9F,0x9C, 0x6,+0 }, // 792: b46M103; b47M103; f20GM103; f31GM103; f36GM103; f48GM103; f49GM103; qGM103; FX 8 sci-fi; gm103
    { 0x014F123,0x0238161, 0x9F,0x00, 0x6,+0 }, // 793: b46M103; b47M103; f20GM103; f31GM103; f36GM103; f48GM103; f49GM103; qGM103; FX 8 sci-fi; gm103
    { 0x053F301,0x1F6F101, 0x46,0x80, 0x0,+0 }, // 794: b46M104; b47M104; f20GM104; f31GM104; f36GM104; f48GM104; f49GM104; qGM104; Sitar; gm104
    { 0x053F201,0x0F6F208, 0x43,0x40, 0x1,+0 }, // 795: b46M104; b47M104; f20GM104; f31GM104; f36GM104; f48GM104; f49GM104; qGM104; Sitar; gm104
    { 0x135A511,0x133A517, 0x10,0xA4, 0x0,+0 }, // 796: b46M105; b47M105; f20GM105; f31GM105; f36GM105; f48GM105; f49GM105; qGM105; Banjo; gm105
    { 0x141F611,0x2E5F211, 0x0D,0x00, 0x0,+0 }, // 797: b46M105; b47M105; f20GM105; f31GM105; f36GM105; f48GM105; f49GM105; qGM105; Banjo; gm105
    { 0x0F8F755,0x1E4F752, 0x92,0x9F, 0xE,+0 }, // 798: b46M106; b47M106; f20GM106; f31GM106; f36GM106; f49GM106; qGM106; Shamisen; gm106
    { 0x0E4F341,0x1E5F351, 0x13,0x00, 0xE,+0 }, // 799: b46M106; b47M106; f20GM106; f31GM106; f36GM106; f49GM106; qGM106; Shamisen; gm106
    { 0x032D493,0x111EB11, 0x91,0x00, 0x8,+0 }, // 800: b46M107; b47M107; f20GM107; f31GM107; f36GM107; qGM107; Koto; gm107
    { 0x032D453,0x112EB13, 0x91,0x0D, 0x9,+0 }, // 801: b46M107; b47M107; f20GM107; f31GM107; f36GM107; qGM107; Koto; gm107
    { 0x3E5F720,0x0E5F521, 0x00,0x0C, 0xD,+0 }, // 802: b46M108; b47M108; f20GM108; f31GM108; f31GM45; f36GM108; f48GM108; f49GM108; qGM108; Kalimba; Pizzicato String; gm108
    { 0x0207C21,0x10C6F22, 0x09,0x09, 0x7,+0 }, // 803: b46M109; b47M109; f20GM109; f31GM109; f36GM109; f48GM109; qGM109; Bagpipe; gm109
    { 0x133DD02,0x0166601, 0x83,0x80, 0xB,+0 }, // 804: b46M110; b47M110; f20GM110; f31GM110; f36GM110; f48GM110; f49GM110; qGM110; Fiddle; gm110
    { 0x0298961,0x406D8A3, 0x33,0xA4, 0x6,+0 }, // 805: b46M111; b47M111; f20GM111; f31GM111; f36GM111; f48GM111; qGM111; Shanai; gm111
    { 0x005DA21,0x00B8F22, 0x17,0x80, 0x6,+0 }, // 806: b46M111; b47M111; f20GM111; f31GM111; f36GM111; f48GM111; qGM111; Shanai; gm111
    { 0x053C601,0x0D5F583, 0x71,0x40, 0x7,+0 }, // 807: b46M112; b47M112; f20GM112; f31GM112; f36GM112; f48GM112; qGM112; Tinkle Bell; gm112
    { 0x026EC08,0x016F804, 0x15,0x00, 0xA,+0 }, // 808: b46M113; b47M113; f20GM113; f31GM113; f36GM113; f48GM113; qGM113; Agogo Bells; gm113
    { 0x026EC07,0x016F802, 0x15,0x00, 0xB,+0 }, // 809: b46M113; b47M113; f20GM113; f31GM113; f36GM113; f48GM113; qGM113; Agogo Bells; gm113
    { 0x024682C,0x035DF01, 0xAB,0x00, 0x0,+0 }, // 810: b46M114; b47M114; f20GM114; f31GM114; f36GM114; f48GM114; f49GM114; qGM114; Steel Drums; gm114
    { 0x0356705,0x005DF01, 0x9D,0x00, 0x1,+0 }, // 811: b46M114; b47M114; f20GM114; f31GM114; f36GM114; f48GM114; f49GM114; qGM114; Steel Drums; gm114
    { 0x4FCFA15,0x0ECFA12, 0x11,0x80, 0xA,+0 }, // 812: b46M115; b47M115; f20GM115; f31GM115; f36GM115; f49GM115; qGM115; Woodblock; gm115
    { 0x0FCFA18,0x0E5F812, 0x9D,0x00, 0xB,+0 }, // 813: b46M115; b47M115; f20GM115; f31GM115; f36GM115; f49GM115; qGM115; Woodblock; gm115
    { 0x007A801,0x083F600, 0x5C,0x03, 0x7,+0 }, // 814: b46M116; b47M116; f20GM116; f31GM116; f36GM116; f49GM116; qGM116; Taiko Drum; gm116
    { 0x458F811,0x0E5F310, 0x8F,0x00, 0xE,+0 }, // 815: b46M117; b47M117; f20GM117; f31GM117; f36GM117; f49GM117; qGM117; Melodic Tom; gm117
    { 0x154F610,0x0E4F410, 0x92,0x00, 0xF,+0 }, // 816: b46M117; b47M117; f20GM117; f31GM117; f36GM117; f49GM117; qGM117; Melodic Tom; gm117
    { 0x455F811,0x0E5F410, 0x86,0x00, 0xE,+0 }, // 817: b46M118; b47M118; f20GM118; f31GM118; f36GM118; f48GM118; f49GM118; qGM118; Synth Drum; gm118
    { 0x155F311,0x0E5F410, 0x9C,0x00, 0xF,+0 }, // 818: b46M118; b47M118; f20GM118; f31GM118; f36GM118; f48GM118; f49GM118; qGM118; Synth Drum; gm118
    { 0x0001F0F,0x3F01FC0, 0x00,0x00, 0xE,+0 }, // 819: b46M119; b47M119; f20GM119; f31GM119; f36GM119; qGM119; Reverse Cymbal; gm119
    { 0x0001F0F,0x3F11FC0, 0x3F,0x3F, 0xF,+0 }, // 820: b46M119; b47M119; f20GM119; f31GM119; f36GM119; qGM119; Reverse Cymbal; gm119
    { 0x024F806,0x7845603, 0x80,0x88, 0xE,+0 }, // 821: b46M120; b47M120; f20GM120; f31GM120; f36GM120; qGM120; Guitar FretNoise; gm120
    { 0x024D803,0x7846604, 0x1E,0x08, 0xF,+0 }, // 822: b46M120; b47M120; f20GM120; f31GM120; f36GM120; qGM120; Guitar FretNoise; gm120
    { 0x001FF06,0x3043414, 0x00,0x00, 0xE,+0 }, // 823: b46M121; b47M121; f20GM121; f31GM121; f36GM121; qGM121; Breath Noise; gm121
    { 0x0F10001,0x0F10001, 0x3F,0x3F, 0xF,+0 }, // 824: b46M121; b46M122; b46M126; b47M121; b47M122; b47M126; b47P88; f20GM121; f20GM122; f20GM126; f31GM121; f31GM122; f31GM126; f36GM121; f36GM122; f36GM126; f49GM122; qGM121; qGM122; qGM126; Applause/Noise; Breath Noise; Seashore; gm121; gm122; gm126; gpo088
    { 0x001FF26,0x1841204, 0x00,0x00, 0xE,+0 }, // 825: b46M122; b47M122; f20GM122; f31GM122; f36GM122; f49GM122; qGM122; Seashore; gm122
    { 0x0F86848,0x0F10001, 0x00,0x3F, 0x5,+0 }, // 826: b46M123; b47M123; f20GM123; f31GM123; f36GM123; f49GM123; qGM123; Bird Tweet; gm123
    { 0x0F86747,0x0F8464C, 0x00,0x00, 0x5,+0 }, // 827: b46M123; b47M123; f20GM123; f31GM123; f36GM123; f49GM123; qGM123; Bird Tweet; gm123
    { 0x261B235,0x015F414, 0x1C,0x08, 0xA,+1 }, // 828: b46M124; b47M124; f20GM124; f31GM124; f36GM124; qGM124; Telephone; gm124
    { 0x715FE11,0x019F487, 0x20,0xC0, 0xB,+0 }, // 829: f20GM124; f31GM124; f36GM124; qGM124; Telephone
    { 0x1112EF0,0x11621E2, 0x00,0xC0, 0x8,-36 }, // 830: b46M125; b47M125; f20GM125; f31GM125; f36GM125; f49GM125; qGM125; Helicopter; gm125
    { 0x7112EF0,0x11621E2, 0x00,0xC0, 0x9,+0 }, // 831: f20GM125; f31GM125; f36GM125; f49GM125; qGM125; Helicopter
    { 0x001FF26,0x71612E4, 0x00,0x00, 0xE,+0 }, // 832: b46M126; b47M126; f20GM126; f31GM126; f36GM126; qGM126; Applause/Noise; gm126
    { 0x059F200,0x000F701, 0x00,0x00, 0xE,+0 }, // 833: b46M127; b47M127; f20GM127; f31GM127; f36GM127; f49GM127; qGM127; Gunshot; gm127
    { 0x0F0F301,0x6C9F601, 0x00,0x00, 0xE,+0 }, // 834: b46M127; b47M127; f20GM127; f31GM127; f36GM127; f49GM127; qGM127; Gunshot; gm127
    { 0x0FEF512,0x0FFF612, 0x11,0xA2, 0x6,+0 }, // 835: b46P37; f20GP37; f31GP37; qGP37; Side Stick; gps037
    { 0x0FFF901,0x0FFF811, 0x0F,0x00, 0x6,+0 }, // 836: b46P37; f20GP37; f31GP37; qGP37; Side Stick; gps037
    { 0x007FC01,0x638F802, 0x03,0x03, 0xF,+0 }, // 837: b46P38; b47P38; b47P40; f20GP38; f31GP38; f36GP38; f36GP40; qGP38; Acoustic Snare; Electric Snare; gpo038; gpo040; gps038
    { 0x204FF82,0x015FF10, 0x00,0x06, 0xE,+0 }, // 838: b46P39; f20GP39; f31GP39; qGP39; Hand Clap; gps039
    { 0x007FF00,0x008FF01, 0x02,0x00, 0xF,+0 }, // 839: b46P39; f20GP39; f31GP39; qGP39; Hand Clap; gps039
    { 0x007FC00,0x638F801, 0x03,0x03, 0xF,+0 }, // 840: b46P40; f20GP40; qGP40; Electric Snare; gps040
    { 0x00CFD01,0x03CD600, 0x07,0x00, 0x0,+0 }, // 841: b46P36; b46P41; b46P43; b46P45; b46P47; b46P48; b46P50; f20GP41; f20GP43; f20GP45; f20GP47; f20GP48; f20GP50; f31GP41; f31GP43; f31GP45; f31GP47; f31GP48; f31GP50; qGP41; qGP43; qGP45; qGP47; qGP48; qGP50; High Floor Tom; High Tom; High-Mid Tom; Low Floor Tom; Low Tom; Low-Mid Tom; gps036; gps041; gps043; gps045; gps047; gps048; gps050
    { 0x00CF600,0x006F600, 0x00,0x00, 0x1,+0 }, // 842: b46P41; f20GP41; f20GP43; f20GP45; f20GP47; f20GP48; f20GP50; f31GP41; f31GP43; f31GP45; f31GP47; f31GP48; f31GP50; qGP41; qGP43; qGP45; qGP47; qGP48; qGP50; High Floor Tom; High Tom; High-Mid Tom; Low Floor Tom; Low Tom; Low-Mid Tom; gps041
    { 0x008F60C,0x247FB12, 0x00,0x00, 0xB,+0 }, // 843: b46P42; f20GP42; f31GP42; qGP42; Closed High Hat; gps042
    { 0x008F60C,0x2477B12, 0x00,0x00, 0xA,+0 }, // 844: b46P44; f20GP44; f31GP44; f48GP44; qGP44; Pedal High Hat; gps044
    { 0x008F60C,0x2477B12, 0x00,0x00, 0xB,+0 }, // 845: b46P44; f20GP44; f31GP44; qGP44; Pedal High Hat; gps044
    { 0x002F60C,0x243CB12, 0x00,0x15, 0xB,+0 }, // 846: b46P46; f20GP46; f31GP46; qGP46; Open High Hat; gps046
    { 0x055F201,0x000F441, 0x00,0x00, 0xE,+0 }, // 847: b46P49; b47P57; b47P59; f20GP49; f31GP49; f36GP57; qGP49; Crash Cymbal 1; Crash Cymbal 2; gpo057; gpo059; gps049
    { 0x000F301,0x0A4F48F, 0x00,0x00, 0xE,+0 }, // 848: b46P49; b47P57; b47P59; f20GP49; f31GP49; f36GP57; qGP49; Crash Cymbal 1; Crash Cymbal 2; gpo057; gpo059; gps049
    { 0x3E4E40F,0x1E5F508, 0x00,0x0A, 0x6,+0 }, // 849: b46P51; f20GP51; f31GP51; f31GP59; qGP51; qGP59; Ride Cymbal 1; Ride Cymbal 2; gps051
    { 0x366F50F,0x1A5F508, 0x00,0x19, 0x7,+0 }, // 850: b46P51; f20GP51; f31GP51; f31GP59; qGP51; qGP59; Ride Cymbal 1; Ride Cymbal 2; gps051
    { 0x065F981,0x030F241, 0x00,0x00, 0xE,+0 }, // 851: b46P52; f20GP52; f31GP52; qGP52; Chinese Cymbal; gps052
    { 0x000FE46,0x055F585, 0x00,0x00, 0xE,+0 }, // 852: b46P52; f20GP52; f31GP52; qGP52; Chinese Cymbal; gps052
    { 0x3E4E40F,0x1E5F507, 0x00,0x11, 0x6,+0 }, // 853: b46P53; f20GP53; f31GP53; qGP53; Ride Bell; gps053
    { 0x365F50F,0x1A5F506, 0x00,0x1E, 0x7,+0 }, // 854: b46P53; f20GP53; f31GP53; qGP53; Ride Bell; gps053
    { 0x0C49406,0x2F5F604, 0x00,0x00, 0x0,+0 }, // 855: b46P54; b47P54; f20GP54; f31GP54; f36GP54; qGP54; Tambourine; gpo054; gps054
    { 0x004F902,0x0F79705, 0x00,0x03, 0x0,+0 }, // 856: b46P54; f20GP54; f31GP54; qGP54; Tambourine; gps054
    { 0x156F28F,0x100F446, 0x03,0x00, 0xE,+0 }, // 857: b46P55; f20GP55; f31GP55; qGP55; Splash Cymbal; gps055
    { 0x000F38F,0x0A5F442, 0x00,0x06, 0xE,+0 }, // 858: b46P55; f20GP55; f31GP55; qGP55; Splash Cymbal; gps055
    { 0x237F811,0x005F310, 0x45,0x00, 0x8,+0 }, // 859: b46P56; b47P56; f20GP56; f31GP56; f36GP56; qGP56; Cow Bell; gpo056; gps056
    { 0x037F811,0x005F310, 0x05,0x08, 0x9,+0 }, // 860: b46P56; f20GP56; f31GP56; qGP56; Cow Bell; gps056
    { 0x155F381,0x000F441, 0x00,0x00, 0xE,+0 }, // 861: b46P57; f20GP57; f31GP57; qGP57; Crash Cymbal 2; gps057
    { 0x000F341,0x0A4F48F, 0x00,0x00, 0xE,+0 }, // 862: b46P57; f20GP57; f31GP57; qGP57; Crash Cymbal 2; gps057
    { 0x503FF80,0x014FF10, 0x00,0x00, 0xC,+0 }, // 863: b46P58; b47P58; f20GP58; f31GP58; f36GP58; qGP58; Vibraslap; gpo058; gps058
    { 0x503FF80,0x014FF10, 0x00,0x0D, 0xD,+0 }, // 864: b46P58; f20GP58; f31GP58; qGP58; Vibraslap; gps058
    { 0x00CF506,0x008F502, 0xC8,0x0B, 0x6,+0 }, // 865: b46P60; f20GP60; f31GP60; qGP60; High Bongo; gps060
    { 0x00CF506,0x007F501, 0xC5,0x03, 0x7,+0 }, // 866: b46P60; f20GP60; f31GP60; qGP60; High Bongo; gps060
    { 0x0BFFA01,0x096C802, 0x8F,0x80, 0x6,+0 }, // 867: b46P61; f20GP61; f31GP61; qGP61; Low Bongo; gps061
    { 0x0BFFA01,0x096C802, 0xCF,0x0B, 0x7,+0 }, // 868: b46P61; f20GP61; f31GP61; qGP61; Low Bongo; gps061
    { 0x087FA01,0x0B7FA01, 0x4F,0x08, 0x7,+0 }, // 869: b46P62; f20GP62; f31GP62; qGP62; Mute High Conga; gps062
    { 0x08DFA01,0x0B5F802, 0x55,0x00, 0x6,+0 }, // 870: b46P63; f20GP63; f31GP63; qGP63; Open High Conga; gps063
    { 0x08DFA01,0x0B5F802, 0x55,0x12, 0x7,+0 }, // 871: b46P63; f20GP63; f31GP63; qGP63; Open High Conga; gps063
    { 0x08DFA01,0x0B6F802, 0x59,0x00, 0x6,+0 }, // 872: b46P64; f20GP64; f31GP64; qGP64; Low Conga; gps064
    { 0x08DFA01,0x0B6F802, 0x59,0x12, 0x7,+0 }, // 873: b46P64; f20GP64; f31GP64; qGP64; Low Conga; gps064
    { 0x00AFA01,0x006F900, 0x00,0x00, 0xE,+0 }, // 874: b46P65; f20GP65; f31GP65; qGP65; High Timbale; gps065
    { 0x00AFA01,0x006F900, 0x00,0x0D, 0xF,+0 }, // 875: b46P65; f20GP65; f31GP65; qGP65; High Timbale; gps065
    { 0x089F900,0x06CF600, 0x80,0x08, 0xF,+0 }, // 876: b46P66; f20GP66; f31GP66; qGP66; Low Timbale; gps066
    { 0x388F803,0x0B6F60C, 0x8D,0x00, 0xE,+0 }, // 877: b46P67; f20GP67; f31GP67; qGP67; High Agogo; gps067
    { 0x088F803,0x0B8F80C, 0x88,0x12, 0xF,+0 }, // 878: b46P67; f20GP67; f31GP67; qGP67; High Agogo; gps067
    { 0x388F803,0x0B6F60C, 0x88,0x03, 0xE,+0 }, // 879: b46P68; f20GP68; f31GP68; qGP68; Low Agogo; gps068
    { 0x388F803,0x0B8F80C, 0x88,0x0F, 0xF,+0 }, // 880: b46P68; f20GP68; f31GP68; qGP68; Low Agogo; gps068
    { 0x04F760F,0x2187700, 0x40,0x08, 0xE,+0 }, // 881: b46P69; b47P69; b47P82; f20GP69; f31GP69; f36GP69; f36GP82; qGP69; Cabasa; Shaker; gpo069; gpo082; gps069
    { 0x04F760F,0x2187700, 0x00,0x12, 0xF,+0 }, // 882: b46P69; f20GP69; f31GP69; qGP69; Cabasa; gps069
    { 0x249C80F,0x2699B02, 0x40,0x80, 0xE,+0 }, // 883: b46P70; f20GP70; f31GP70; qGP70; Maracas; gps070
    { 0x249C80F,0x2699B0F, 0xC0,0x19, 0xF,+0 }, // 884: b46P70; f20GP70; f31GP70; qGP70; Maracas; gps070
    { 0x305AD57,0x0058D87, 0xDC,0x00, 0xE,+0 }, // 885: b46P71; f20GP71; f31GP71; qGP71; Short Whistle; gps071
    { 0x305AD47,0x0058D87, 0xDC,0x12, 0xF,+0 }, // 886: b46P71; f20GP71; f31GP71; qGP71; Short Whistle; gps071
    { 0x304A857,0x0048887, 0xDC,0x00, 0xE,+0 }, // 887: b46P72; f20GP72; f31GP72; qGP72; Long Whistle; gps072
    { 0x304A857,0x0058887, 0xDC,0x08, 0xF,+0 }, // 888: b46P72; f20GP72; f31GP72; qGP72; Long Whistle; gps072
    { 0x506F680,0x016F610, 0x00,0x00, 0xC,+0 }, // 889: b46P73; b46P74; f20GP73; f20GP74; f31GP73; f31GP74; qGP73; qGP74; Long Guiro; Short Guiro; gps073; gps074
    { 0x50F6F00,0x50F6F00, 0x00,0x00, 0xD,+0 }, // 890: b46P73; b47P73; f20GP73; f31GP73; f36GP73; qGP73; Short Guiro; gpo073; gps073
    { 0x50F4F00,0x50F4F00, 0x00,0x00, 0xD,+0 }, // 891: b46P74; b47P74; f20GP74; f31GP74; f36GP74; qGP74; Long Guiro; gpo074; gps074
    { 0x3F40006,0x0F5F715, 0x3F,0x00, 0x0,+0 }, // 892: b46P75; b47P39; b47P75; b47P76; b47P77; b47P85; f20GP75; f31GP75; f36GP39; f36GP75; f36GP76; f36GP77; f36GP85; qGP75; Castanets; Claves; Hand Clap; High Wood Block; Low Wood Block; gpo039; gpo075; gpo076; gpo077; gpo085; gps075
    { 0x3F40006,0x0F5F715, 0x3F,0x08, 0x1,+0 }, // 893: b46P75; f20GP75; f31GP75; qGP75; Claves; gps075
    { 0x3F40006,0x0F5F712, 0x3F,0x08, 0x1,+0 }, // 894: b46P76; b46P77; f20GP76; f20GP77; f31GP76; f31GP77; qGP76; qGP77; High Wood Block; Low Wood Block; gps076; gps077
    { 0x7476701,0x0476703, 0xCD,0x40, 0x8,+0 }, // 895: b46P78; f20GP78; f31GP78; qGP78; Mute Cuica; gps078
    { 0x0476701,0x0556501, 0xC0,0x00, 0x9,+0 }, // 896: b46P78; f20GP78; f31GP78; qGP78; Mute Cuica; gps078
    { 0x0A76701,0x0356503, 0x17,0x1E, 0xA,+0 }, // 897: b46P79; f20GP79; f31GP79; qGP79; Open Cuica; gps079
    { 0x0777701,0x0057501, 0x9D,0x00, 0xB,+0 }, // 898: b46P79; f20GP79; f31GP79; qGP79; Open Cuica; gps079
    { 0x3F0E00A,0x005FF1F, 0x40,0x40, 0x8,+0 }, // 899: b46P80; f20GP80; f31GP80; qGP80; Mute Triangle; gps080
    { 0x3F0E00A,0x005FF1F, 0x40,0x48, 0x9,+0 }, // 900: b46P80; f20GP80; f31GP80; qGP80; Mute Triangle; gps080
    { 0x3F0E00A,0x002FF1F, 0x7C,0x40, 0x8,+0 }, // 901: b46P81; f20GP81; f31GP81; qGP81; Open Triangle; gps081
    { 0x3E0F50A,0x003FF1F, 0x7C,0x40, 0x9,+0 }, // 902: b46P81; f20GP81; f31GP81; qGP81; Open Triangle; gps081
    { 0x04F7F0F,0x21E7E00, 0x40,0x88, 0xE,+0 }, // 903: b46P82; f20GP82; f31GP82; qGP82; Shaker; gps082
    { 0x04F7F0F,0x21E7E00, 0x40,0x14, 0xF,+0 }, // 904: b46P82; f20GP82; f31GP82; qGP82; Shaker; gps082
    { 0x332F905,0x0A6D604, 0x05,0x40, 0xE,+0 }, // 905: b46P83; f20GP83; f31GP83; qGP83; Jingle Bell; gps083
    { 0x332F805,0x0A67404, 0x05,0x40, 0xF,+0 }, // 906: b46P83; f20GP83; f31GP83; qGP83; Jingle Bell; gps083
    { 0x6E5E403,0x7E7F507, 0x0D,0x11, 0xB,+0 }, // 907: b46P84; f20GP84; f31GP84; f36GP84; qGP84; Bell Tree; gps084
    { 0x366F500,0x4A8F604, 0x1B,0x15, 0xA,+0 }, // 908: b46P84; f20GP84; f31GP84; f36GP84; qGP84; Bell Tree; gps084
    { 0x3F40003,0x0F5F715, 0x3F,0x00, 0x8,+0 }, // 909: b46P85; f20GP85; f31GP85; qGP85; Castanets; gps085
    { 0x3F40003,0x0F5F715, 0x3F,0x08, 0x9,+0 }, // 910: b46P85; f20GP85; f31GP85; qGP85; Castanets; gps085
    { 0x08DFA01,0x0B5F802, 0x4F,0x00, 0x6,+0 }, // 911: b46P86; b47P64; f20GP86; f31GP86; f36GP64; qGP86; Low Conga; Mute Surdu; gpo064; gps086
    { 0x08DFA01,0x0B5F802, 0x4F,0x12, 0x7,+0 }, // 912: b46P86; f20GP86; f31GP86; qGP86; Mute Surdu; gps086
    { 0x084FA01,0x0B4F800, 0x4F,0x00, 0x6,+0 }, // 913: b46P87; f20GP87; f31GP87; qGP87; Open Surdu; gps087
    { 0x084FA01,0x0B4F800, 0x4F,0x00, 0x7,+0 }, // 914: b46P87; f20GP87; f31GP87; qGP87; Open Surdu; gps087
    { 0x045F221,0x076F221, 0x8F,0x06, 0x8,+0 }, // 915: f17GM0; mGM0; AcouGrandPiano
    { 0x03BF271,0x00BF3A1, 0x0E,0x00, 0x6,+0 }, // 916: f17GM3; f35GM3; mGM3; Honky-tonkPiano
    { 0x054F60C,0x0B5F341, 0x5C,0x00, 0x0,+0 }, // 917: f17GM8; f35GM8; mGM8; Celesta
    { 0x0E6F318,0x0F6F241, 0x62,0x00, 0x0,+0 }, // 918: f17GM11; mGM11; Vibraphone
    { 0x082D385,0x0E3A341, 0x59,0x80, 0xC,+0 }, // 919: f17GM14; f35GM14; mGM14; Tubular Bells
    { 0x1557403,0x005B341, 0x49,0x80, 0x4,+0 }, // 920: f17GM15; mGM15; Dulcimer
    { 0x014F6B1,0x007F131, 0x92,0x00, 0x2,+0 }, // 921: f17GM16; f29GM46; f30GM46; mGM16; Hammond Organ; Orchestral Harp
    { 0x058C7B2,0x008C730, 0x14,0x00, 0x2,+0 }, // 922: f17GM17; mGM17; Percussive Organ
    { 0x018AAB0,0x0088A71, 0x44,0x00, 0x4,+0 }, // 923: f17GM18; f29GM10; f30GM10; mGM18; Music box; Rock Organ
    { 0x1239723,0x0145571, 0x93,0x00, 0x4,+0 }, // 924: f17GM19; f29GM12; f29GM13; f29GM14; f30GM12; f30GM13; f30GM14; mGM19; Church Organ; Marimba; Tubular Bells; Xylophone
    { 0x10497A1,0x0045571, 0x13,0x80, 0x0,+0 }, // 925: f17GM20; f35GM20; mGM20; Reed Organ
    { 0x12A9824,0x01A4671, 0x48,0x00, 0xC,+0 }, // 926: f15GM15; f17GM21; f26GM15; f29GM15; f30GM15; f35GM21; mGM21; Accordion; Dulcimer
    { 0x10691A1,0x0076121, 0x13,0x00, 0xA,+0 }, // 927: f17GM22; f29GM87; f30GM87; mGM22; Harmonica; Lead 8 brass
    { 0x0067121,0x0076161, 0x13,0x89, 0x6,+0 }, // 928: f17GM23; f35GM23; mGM23; Tango Accordion
    { 0x194F302,0x0C8F381, 0x9C,0x80, 0xC,+0 }, // 929: f17GM24; f30GM59; mGM24; Acoustic Guitar1; Muted Trumpet
    { 0x04F2009,0x0F8D144, 0xA1,0x80, 0x8,+0 }, // 930: f17GM31; mGM31; Guitar Harmonics
    { 0x0069421,0x0A6C362, 0x1E,0x00, 0x2,+0 }, // 931: f17GM32; f29GM65; f30GM65; mGM32; Acoustic Bass; Alto Sax
    { 0x11CD1B1,0x00C6131, 0x49,0x00, 0x8,+0 }, // 932: f17GM42; f29GM54; f29GM55; f30GM54; f30GM55; mGM42; Cello; Orchestra Hit; Synth Voice
    { 0x125B121,0x0087262, 0x9B,0x01, 0xE,+0 }, // 933: f17GM48; f30GM50; mGM48; String Ensemble1; Synth Strings 1
    { 0x1037F61,0x1073F21, 0x98,0x00, 0x0,+0 }, // 934: f17GM49; mGM49; String Ensemble2
    { 0x012C161,0x0054FA1, 0x93,0x00, 0xA,+0 }, // 935: f17GM50; f29GM48; f30GM48; mGM50; String Ensemble1; Synth Strings 1
    { 0x022C121,0x0054FA1, 0x18,0x00, 0xC,+0 }, // 936: f17GM51; f29GM49; f30GM49; mGM51; String Ensemble2; SynthStrings 2
    { 0x015F431,0x0058AB2, 0x5B,0x83, 0x0,+0 }, // 937: f17GM52; f29GM34; f29GM35; f30GM33; f30GM34; f30GM35; f35GM52; mGM52; Choir Aahs; Electric Bass 1; Electric Bass 2; Fretless Bass
    { 0x0397461,0x06771A1, 0x90,0x00, 0x0,+0 }, // 938: f17GM53; f35GM53; mGM53; Voice Oohs
    { 0x00554B1,0x0057AB2, 0x57,0x00, 0xC,+0 }, // 939: f17GM54; f29GM39; f30GM39; f35GM54; mGM54; Synth Bass 2; Synth Voice
    { 0x0635450,0x045A581, 0x00,0x00, 0x8,+0 }, // 940: f17GM55; f29GM122; f30GM122; mGM55; Orchestra Hit; Seashore
    { 0x0157621,0x03782A1, 0x94,0x00, 0xC,+0 }, // 941: f17GM58; f29GM94; f30GM94; mGM58; Pad 7 halo; Tuba
    { 0x01F75A1,0x00F7422, 0x8A,0x06, 0x8,+0 }, // 942: f17GM61; mGM61; Brass Section
    { 0x1557261,0x0187121, 0x86,0x0D, 0x0,+0 }, // 943: f17GM62; mGM62; Synth Brass 1
    { 0x1029331,0x00B72A1, 0x8F,0x00, 0x8,+0 }, // 944: f17GM64; f35GM64; mGM64; Soprano Sax
    { 0x1039331,0x00982A1, 0x91,0x00, 0xA,+0 }, // 945: f17GM66; f35GM66; mGM66; Tenor Sax
    { 0x10F9331,0x00F72A1, 0x8E,0x00, 0xA,+0 }, // 946: f17GM67; f29GM81; f30GM81; f35GM67; mGM67; Baritone Sax; Lead 2 sawtooth
    { 0x01F7561,0x00A7521, 0x9C,0x00, 0x2,+0 }, // 947: f17GM74; f30GM76; mGM74; Bottle Blow; Recorder
    { 0x05666E1,0x0266561, 0x4C,0x00, 0x0,+0 }, // 948: f17GM76; f29GM109; f29GM110; f29GM80; f30GM109; f30GM110; f30GM80; f35GM76; mGM76; Bagpipe; Bottle Blow; Fiddle; Lead 1 squareea
    { 0x04676A2,0x0365561, 0xCB,0x00, 0x0,+0 }, // 949: f17GM77; f29GM107; f30GM107; mGM77; Koto; Shakuhachi
    { 0x00757A2,0x0075661, 0x99,0x00, 0xB,+0 }, // 950: f17GM78; f29GM108; f30GM108; mGM78; Kalimba; Whistle
    { 0x00777A2,0x0077661, 0x93,0x00, 0xB,+0 }, // 951: f17GM79; f29GM111; f30GM111; f35GM79; mGM79; Ocarina; Shanai
    { 0x0126621,0x00A9661, 0x45,0x00, 0x0,+0 }, // 952: f17GM83; f35GM83; mGM83; Lead 4 chiff
    { 0x005DF62,0x0076FA1, 0x9E,0x40, 0x2,+0 }, // 953: f17GM85; f35GM85; mGM85; Lead 6 voice
    { 0x001EF20,0x2068FA0, 0x1A,0x00, 0x0,+0 }, // 954: f17GM86; f35GM86; mGM86; Lead 7 fifths
    { 0x09453B7,0x005A061, 0xA5,0x00, 0x2,+0 }, // 955: f17GM88; f29GM32; f30GM32; mGM88; Acoustic Bass; Pad 1 new age
    { 0x011A8A1,0x0032571, 0x1F,0x80, 0xA,+0 }, // 956: f17GM89; f35GM89; mGM89; Pad 2 warm
    { 0x03491A1,0x01655A1, 0x17,0x00, 0xC,+0 }, // 957: f17GM90; mGM90; Pad 3 polysynth
    { 0x00154B1,0x0036AB2, 0x5D,0x00, 0x0,+0 }, // 958: f17GM91; f35GM91; mGM91; Pad 4 choir
    { 0x0432121,0x0354262, 0x97,0x00, 0x8,+0 }, // 959: f17GM92; f35GM92; mGM92; Pad 5 bowedpad
    { 0x177A161,0x1473121, 0x1C,0x00, 0x0,+0 }, // 960: f17GM93; f35GM93; mGM93; Pad 6 metallic
    { 0x0331121,0x02542A1, 0x89,0x03, 0xA,+0 }, // 961: f17GM94; f35GM94; mGM94; Pad 7 halo
    { 0x1471161,0x007CF21, 0x15,0x00, 0x0,+0 }, // 962: f17GM95; f35GM95; mGM95; Pad 8 sweep
    { 0x0F6F83A,0x0028691, 0xCE,0x00, 0x2,+0 }, // 963: f17GM96; f29GM41; f29GM43; f30GM41; f30GM43; mGM96; Contrabass; FX 1 rain; Viola
    { 0x081B122,0x026F2A1, 0x92,0x83, 0xC,+0 }, // 964: f17GM99; f29GM37; f30GM37; mGM99; FX 4 atmosphere; Slap Bass 2
    { 0x151F181,0x0F5F282, 0x4D,0x00, 0x0,+0 }, // 965: f17GM100; f35GM100; mGM100; FX 5 brightness
    { 0x15111A1,0x0131163, 0x94,0x80, 0x6,+0 }, // 966: f17GM101; f35GM101; mGM101; FX 6 goblins
    { 0x03111A1,0x0031D61, 0x8C,0x80, 0x6,+0 }, // 967: f17GM102; f35GM102; mGM102; FX 7 echoes
    { 0x173F364,0x02381A1, 0x4C,0x00, 0x4,+0 }, // 968: f17GM103; f35GM103; mGM103; FX 8 sci-fi
    { 0x032D453,0x111EB51, 0x91,0x00, 0x8,+0 }, // 969: f17GM107; f29GM105; f30GM105; f35GM107; mGM107; Banjo; Koto
    { 0x000F68E,0x3029F5E, 0x00,0x00, 0xE,+0 }, // 970: f17GP55; f29GP55; f30GP55; f35GP55; f49GP55; mGP55; Splash Cymbal
    { 0x303FF40,0x014FF10, 0x00,0x0D, 0xC,+0 }, // 971: f17GP58; f29GP58; f30GP58; f35GP58; f49GP58; mGP58; Vibraslap
    { 0x306F640,0x3176711, 0x00,0x00, 0xE,+0 }, // 972: f17GP73; f29GP73; f30GP73; f35GP73; f49GP73; mGP73; Short Guiro
    { 0x205F540,0x3164611, 0x00,0x09, 0xE,+0 }, // 973: f17GP74; f29GP74; f30GP74; f49GP74; mGP74; Long Guiro
    { 0x048F881,0x0057582, 0x45,0x08, 0x0,+0 }, // 974: f17GP79; f29GP79; f30GP79; f35GP79; f48GP79; f49GP79; mGP79; Open Cuica
    { 0x053F101,0x074D211, 0x4F,0x00, 0x6,+0 }, // 975: MGM0; MGM7; f19GM0; f19GM7; f21GM0; f21GM7; f23GM7; f32GM0; f32GM7; f37GM0; f41GM0; f41GM7; f47GM1; AcouGrandPiano; BrightAcouGrand; Clavinet
    { 0x050F101,0x07CD201, 0x4F,0x04, 0x6,+0 }, // 976: MGM1; MGM3; MGM5; b41M1; b41M5; f19GM1; f19GM5; f21GM1; f23GM3; f23GM5; f32GM1; f32GM3; f32GM5; f41GM1; f47GM5; BrightAcouGrand; Chorused Piano; Honky-tonkPiano; elpiano1; piano3.i
    { 0x060F101,0x07BD211, 0x4D,0x00, 0x8,+0 }, // 977: MGM2; f23GM2; f32GM2; f47GM3; ElecGrandPiano; Honky-tonkPiano
    { 0x013F202,0x043F502, 0x22,0x00, 0xE,+0 }, // 978: MGM4; f23GM4; f32GM4; Rhodes Piano
    { 0x053F101,0x083F212, 0xCF,0x00, 0x2,+0 }, // 979: MGM6; b41M6; f19GM6; f21GM6; f23GM6; f32GM6; Harpsichord; pianof.i
    { 0x00FFF24,0x00FFF21, 0x80,0x80, 0x1,+0 }, // 980: MGM8; f23GM8; f32GM8; f35GM17; Celesta; Percussive Organ
    { 0x00381A5,0x005F1B2, 0xD2,0x80, 0x2,+0 }, // 981: MGM10; MGM9; f19GM9; f23GM10; f23GM9; Glockenspiel; Music box
    { 0x0F0FB3E,0x09BA0B1, 0x29,0x40, 0x0,+0 }, // 982: MGM11; f23GM11; f35GM16; oGM11; Hammond Organ; Vibraphone
    { 0x00381A5,0x005F1B1, 0xD2,0x40, 0x2,+0 }, // 983: MGM12; MGM13; MGM14; f23GM12; f23GM13; f23GM14; Marimba; Tubular Bells; Xylophone
    { 0x00BF224,0x00B5231, 0x4F,0x00, 0xE,+0 }, // 984: MGM15; b41M63; f19GM63; f23GM15; f32GM15; f41GM63; Dulcimer; Synth Brass 2; accordn.
    { 0x001F211,0x0B1F215, 0x0D,0x0D, 0xA,+0 }, // 985: MGM16; MGM17; MGM18; b41M18; f19GM16; f19GM17; f19GM18; f21GM18; f23GM16; f23GM17; f23GM18; f32GM16; f32GM17; f32GM18; f41GM18; Hammond Organ; Percussive Organ; Rock Organ; harpsi4.
    { 0x153F101,0x274F111, 0x49,0x00, 0x6,+0 }, // 986: MGM19; MGM20; MGM21; b41M20; f12GM7; f16GM7; f19GM20; f21GM20; f23GM19; f23GM20; f23GM21; f32GM19; f32GM20; f32GM21; f41GM20; f54GM7; Accordion; Church Organ; Clavinet; Reed Organ; elclav2.
    { 0x0100133,0x0337D14, 0x87,0x80, 0x8,+0 }, // 987: MGM22; MGM23; f19GM22; f19GM23; f23GM22; f23GM23; f32GM22; f32GM23; Harmonica; Tango Accordion
    { 0x0AE71E1,0x09E81E1, 0x16,0x00, 0xA,+0 }, // 988: MGM24; MGM25; MGM26; MGM27; f19GM27; f32GM24; f32GM25; f32GM26; f32GM27; oGM26; Acoustic Guitar1; Acoustic Guitar2; Electric Guitar1; Electric Guitar2
    { 0x0EFF201,0x078F501, 0x1D,0x00, 0xA,+0 }, // 989: MGM28; MGM29; MGM30; MGM31; MGM44; MGM45; MGM65; MGM66; MGM67; b41M29; b41M30; b41M31; b41M44; b41M45; b41M65; b41M66; f15GM65; f19GM28; f19GM29; f19GM30; f19GM31; f19GM44; f19GM45; f19GM65; f19GM66; f19GM67; f21GM30; f21GM31; f23GM28; f23GM29; f23GM31; f23GM44; f23GM45; f26GM65; f32GM28; f32GM29; f32GM30; f32GM31; f32GM44; f32GM45; f32GM65; f32GM66; f32GM67; f41GM29; f41GM30; f41GM31; f41GM44; f41GM45; f41GM65; f41GM66; f41GM67; f47GM34; oGM28; oGM29; oGM30; oGM31; oGM44; oGM45; oGM65; oGM66; oGM67; Alto Sax; Baritone Sax; Distorton Guitar; Electric Bass 2; Electric Guitar3; Guitar Harmonics; Overdrive Guitar; Pizzicato String; Tenor Sax; Tremulo Strings; bass2.in
    { 0x002A474,0x04245D7, 0x47,0x40, 0x6,+0 }, // 990: MGM32; f23GM32; Acoustic Bass
    { 0x05331C5,0x07242D9, 0x8F,0x00, 0x6,+0 }, // 991: MGM125; MGM33; MGM36; f23GM33; f23GM36; f32GM125; f32GM33; f32GM36; f53GM125; Electric Bass 1; Helicopter; Slap Bass 1
    { 0x0022A95,0x0F34212, 0x97,0x80, 0x0,+0 }, // 992: MGM34; f19GM34; f23GM34; Electric Bass 2
    { 0x001EF4F,0x0F19801, 0x81,0x00, 0x4,+0 }, // 993: MGM35; f19GM35; f23GM35; Fretless Bass
    { 0x019D530,0x01B61B1, 0x88,0x80, 0xC,+0 }, // 994: MGM39; f19GM39; f23GM39; oGM39; Synth Bass 2
    { 0x060F207,0x072F212, 0x4F,0x00, 0x8,+0 }, // 995: MGM126; MGM40; MGM41; b41M40; f19GM40; f23GM126; f23GM40; f23GM41; f32GM126; f32GM40; f32GM41; f35GM112; f41GM40; Applause/Noise; Tinkle Bell; Viola; Violin; bells.in
    { 0x0176E71,0x00E8B22, 0xC5,0x05, 0x2,+0 }, // 996: MGM42; f19GM42; f23GM42; oGM42; Cello
    { 0x02F2501,0x06C6521, 0x15,0x80, 0xA,+0 }, // 997: MGM43; MGM70; MGM71; b41M70; f19GM70; f23GM43; f23GM70; f32GM43; f32GM70; f32GM71; f41GM70; Bassoon; Clarinet; Contrabass; bass1.in
    { 0x0427847,0x0548554, 0x4D,0x00, 0xA,+0 }, // 998: MGM46; f19GM46; f23GM46; oGM46; Orchestral Harp
    { 0x011F111,0x0B3F101, 0x4A,0x88, 0x6,+0 }, // 999: MGM47; f19GM47; f23GM47; f32GM47; Timpany
    { 0x0117171,0x11542A1, 0x8B,0x40, 0x6,+0 }, // 1000: MGM48; MGM50; String Ensemble1; Synth Strings 1
    { 0x3D3B1E1,0x1741221, 0x4F,0x00, 0x6,+0 }, // 1001: MGM49; f23GM49; f32GM49; String Ensemble2
    { 0x053090E,0x094F702, 0x80,0x00, 0xE,+0 }, // 1002: MGM105; MGM51; f32GM105; f32GM51; oGM105; Banjo; SynthStrings 2
    { 0x0035172,0x0135262, 0x1C,0x05, 0xE,+0 }, // 1003: MGM52; MGM54; f19GM52; f19GM54; f23GM52; f23GM54; oGM52; oGM54; Choir Aahs; Synth Voice
    { 0x0035131,0x06754A1, 0x1C,0x80, 0xE,+0 }, // 1004: MGM53; f19GM53; f23GM53; f35GM40; Violin; Voice Oohs
    { 0x0115270,0x0FE3171, 0xC5,0x40, 0x0,+0 }, // 1005: MGM55; f19GM55; f23GM55; Orchestra Hit
    { 0x0B69401,0x0268300, 0x00,0x00, 0x0,+0 }, // 1006: MGM56; b41M56; f19GM56; f21GM56; f23GM56; f32GM56; f41GM56; Trumpet; contrab.
    { 0x075F502,0x0F3F201, 0x29,0x83, 0x0,+0 }, // 1007: MGM57; b41M57; f19GM57; f21GM57; f23GM57; f25GM57; f32GM57; f41GM57; f47GM46; Orchestral Harp; Trombone; harp1.in
    { 0x243A321,0x022C411, 0x11,0x00, 0xC,+0 }, // 1008: MGM58; b41M58; f19GM58; f21GM58; f23GM58; f32GM58; f41GM58; Tuba; harp.ins
    { 0x01FF201,0x088F501, 0x11,0x00, 0xA,+0 }, // 1009: MGM59; MGM60; b41M59; b41M60; f19GM59; f19GM60; f21GM59; f21GM60; f23GM59; f23GM60; f32GM59; f32GM60; f41GM59; French Horn; Muted Trumpet; guitar1.
    { 0x021FF13,0x003FF11, 0x96,0x80, 0xA,+0 }, // 1010: MGM61; f12GM27; f16GM27; f19GM61; f23GM61; f32GM61; f54GM27; Brass Section; Electric Guitar2
    { 0x01797F1,0x018F121, 0x01,0x0D, 0x8,+0 }, // 1011: MGM62; f19GM62; f23GM62; f32GM62; Synth Brass 1
    { 0x053F101,0x053F108, 0x40,0x40, 0x0,+0 }, // 1012: MGM63; f23GM63; f32GM63; Synth Brass 2
    { 0x021FF13,0x003FF10, 0x51,0x40, 0xA,+0 }, // 1013: MGM64; MGM68; MGM69; b41M64; b41M68; b41M69; f19GM64; f19GM68; f19GM69; f32GM64; f32GM68; f32GM69; f41GM64; f41GM68; f41GM69; oGM68; oGM69; English Horn; Oboe; Soprano Sax; bbass.in
    { 0x08F7461,0x02A6561, 0x27,0x80, 0x2,+0 }, // 1014: MGM72; MGM73; MGM74; MGM75; f35GM73; Flute; Pan Flute; Piccolo; Recorder
    { 0x08F6EE0,0x02A65A1, 0xEC,0x00, 0xE,+0 }, // 1015: MGM110; MGM111; MGM76; MGM77; f19GM110; f19GM111; f35GM74; oGM110; oGM111; oGM77; Bottle Blow; Fiddle; Recorder; Shakuhachi; Shanai
    { 0x0537101,0x07C5212, 0x4F,0x00, 0xA,+0 }, // 1016: MGM78; MGM79; MGM80; b41M79; b41M80; f19GM78; f19GM79; f19GM80; f23GM79; f32GM78; f32GM79; f32GM80; Lead 1 squareea; Ocarina; Whistle; sax1.ins
    { 0x0667150,0x08B5290, 0x92,0x00, 0xE,+0 }, // 1017: MGM81; Lead 2 sawtooth
    { 0x0247332,0x0577521, 0x16,0x80, 0xE,+0 }, // 1018: MGM82; f32GM82; Lead 3 calliope
    { 0x01B5132,0x03BA2A1, 0x9A,0x82, 0xC,+0 }, // 1019: MGM83; f19GM83; f35GM71; Clarinet; Lead 4 chiff
    { 0x0176E71,0x00E8B62, 0xC5,0x05, 0x2,+0 }, // 1020: MGM84; MGM85; f19GM84; f19GM85; Lead 5 charang; Lead 6 voice
    { 0x019D530,0x01B61B1, 0xCD,0x40, 0xC,+0 }, // 1021: MGM86; f19GM86; f35GM70; Bassoon; Lead 7 fifths
    { 0x2034122,0x10561F2, 0x4F,0x80, 0x2,+0 }, // 1022: MGM87; f23GM87; f32GM87; f35GM22; oGM87; Harmonica; Lead 8 brass
    { 0x00B4131,0x03B92A1, 0x1C,0x80, 0xC,+0 }, // 1023: MGM88; MGM89; f19GM88; f19GM89; f23GM89; f35GM56; Pad 1 new age; Pad 2 warm; Trumpet
    { 0x01D5321,0x03B52A1, 0x1C,0x80, 0xC,+0 }, // 1024: MGM90; f19GM90; f23GM90; Pad 3 polysynth
    { 0x01F4171,0x03B92A1, 0x1C,0x80, 0xE,+0 }, // 1025: MGM91; f19GM91; f25GM90; f35GM57; Pad 3 polysynth; Pad 4 choir; Trombone
    { 0x05A5321,0x01AAA21, 0x9F,0x80, 0xC,+0 }, // 1026: MGM92; b41M92; f19GM92; f25GM92; f32GM92; f41GM92; f47GM60; f53GM93; French Horn; Pad 5 bowedpad; Pad 6 metallic; frhorn1.
    { 0x28FA520,0x03D3621, 0x8E,0x00, 0x6,+0 }, // 1027: MGM93; f32GM93; Pad 6 metallic
    { 0x08C4321,0x02F8521, 0x19,0x80, 0xC,+0 }, // 1028: MGM94; f32GM94; Pad 7 halo
    { 0x0AE7121,0x09E8121, 0x16,0x00, 0xE,+0 }, // 1029: MGM95; f12GM63; f16GM63; f19GM95; f32GM95; f47GM62; f54GM63; oGM95; Pad 8 sweep; Synth Brass 1; Synth Brass 2
    { 0x0AE7161,0x02E8160, 0x1C,0x00, 0xE,+0 }, // 1030: MGM96; f23GM96; oGM96; FX 1 rain
    { 0x054F606,0x0B3F241, 0x73,0x03, 0x0,+0 }, // 1031: MGM97; FX 2 soundtrack
    { 0x212AA53,0x021AC51, 0x97,0x80, 0xE,+0 }, // 1032: MGM98; f19GM98; FX 3 crystal
    { 0x025DA05,0x015F901, 0x8E,0x00, 0xA,+0 }, // 1033: MGM104; MGM99; b41M104; b41M99; f19GM104; f19GM99; f32GM104; f32GM99; oGM104; oGM99; FX 4 atmosphere; Sitar; marimba.
    { 0x001FFA4,0x0F3F53E, 0xDB,0xC0, 0x4,+0 }, // 1034: MGM100; MGM101; MGM102; f19GM101; f19GM102; f23GM102; oGM100; oGM101; oGM102; FX 5 brightness; FX 6 goblins; FX 7 echoes
    { 0x22F55B0,0x31E87E0, 0x16,0x80, 0xC,+0 }, // 1035: MGM106; f15GM106; f19GM106; f26GM106; oGM106; Shamisen
    { 0x0177421,0x0176562, 0x83,0x8D, 0x7,+0 }, // 1036: MGM107; MGM108; MGM109; oGM108; oGM109; Bagpipe; Kalimba; Koto
    { 0x0EFE800,0x0FFA500, 0x0D,0x00, 0x6,+0 }, // 1037: MGM112; b41M112; f19GM112; f21GM112; f32GM112; f41GM112; Tinkle Bell; bdrum3.i
    { 0x2A2B264,0x1D49703, 0x02,0x80, 0xE,+0 }, // 1038: MGM119; f19GM119; Reverse Cymbal
    { 0x0F3F8E2,0x0F3F770, 0x86,0x40, 0x4,+0 }, // 1039: MGM120; f19GM120; Guitar FretNoise
    { 0x0F0E026,0x031FF1E, 0x03,0x00, 0x8,+0 }, // 1040: MGM121; f19GM121; f32GM121; Breath Noise
    { 0x0056541,0x0743291, 0x83,0x00, 0xA,+0 }, // 1041: MGM122; f19GM122; Seashore
    { 0x061F217,0x0B2F112, 0x4F,0x08, 0x8,+0 }, // 1042: MGM123; f19GM123; f21GM123; f32GM123; f41GM123; oGM123; Bird Tweet
    { 0x0F0F000,0x0F05F0C, 0x2E,0x00, 0xE,+0 }, // 1043: MGM124; f23GM124; f32GM124; f35GM123; Bird Tweet; Telephone
    { 0x0031801,0x090F674, 0x80,0xC1, 0xE,+0 }, // 1044: MGM127; oGM127; Gunshot
    { 0x04CA800,0x04FD600, 0x0B,0x03, 0x0,+0 }, // 1045: MGP35; MGP36; f32GP35; f32GP36; Ac Bass Drum; Bass Drum 1
    { 0x00FFF2E,0x04CF600, 0x00,0x18, 0xE,+0 }, // 1046: MGP38; MGP39; MGP40; MGP67; MGP68; b41P39; f19GP39; f32GP38; f32GP39; f32GP40; f32GP67; f32GP68; f41GP39; Acoustic Snare; Electric Snare; Hand Clap; High Agogo; Low Agogo; snare2.i
    { 0x282B264,0x1DA9803, 0x00,0x93, 0xE,+0 }, // 1047: MGP42; Closed High Hat
    { 0x282B264,0x1D49703, 0x00,0x80, 0xE,+0 }, // 1048: MGP44; MGP46; MGP51; MGP54; MGP69; MGP70; MGP71; MGP72; MGP73; MGP75; f19GP44; f19GP46; f19GP47; f19GP69; f19GP70; f19GP71; f19GP72; f19GP73; f19GP75; f23GP44; f23GP46; f23GP69; f23GP71; f23GP72; f23GP73; f23GP75; Cabasa; Claves; Long Whistle; Low-Mid Tom; Maracas; Open High Hat; Pedal High Hat; Ride Cymbal 1; Short Guiro; Short Whistle; Tambourine
    { 0x0A0B264,0x1D69603, 0x02,0x80, 0xE,+0 }, // 1049: MGP49; Crash Cymbal 1
    { 0x053F101,0x074F111, 0x4B,0x00, 0x6,+0 }, // 1050: oGM0; oGM1; oGM2; AcouGrandPiano; BrightAcouGrand; ElecGrandPiano
    { 0x0117F27,0x0441122, 0x0E,0x00, 0xE,+0 }, // 1051: oGM3; Honky-tonkPiano
    { 0x0111122,0x0121123, 0x15,0x00, 0x4,+0 }, // 1052: oGM4; Rhodes Piano
    { 0x053F101,0x074F111, 0x59,0x00, 0x6,+0 }, // 1053: oGM5; Chorused Piano
    { 0x0FFF691,0x0F4F511, 0x00,0x00, 0x8,+0 }, // 1054: oGM6; Harpsichord
    { 0x3087631,0x00F6531, 0x08,0x00, 0x2,+0 }, // 1055: oGM7; Clavinet
    { 0x019D083,0x017F002, 0x5D,0x80, 0xA,+0 }, // 1056: oGM8; Celesta
    { 0x019D083,0x017F002, 0x58,0x80, 0xA,+0 }, // 1057: oGM9; Glockenspiel
    { 0x013F6A6,0x005F1B1, 0xE5,0x40, 0x2,+0 }, // 1058: oGM10; Music box
    { 0x1239722,0x013457A, 0x44,0x00, 0x4,+0 }, // 1059: oGM12; Marimba
    { 0x1239721,0x0134572, 0x8A,0x80, 0x2,+0 }, // 1060: oGM13; Xylophone
    { 0x0FFF4F1,0x06FF2F1, 0x02,0x00, 0x0,+0 }, // 1061: oGM14; Tubular Bells
    { 0x00F3FF1,0x06FF2F1, 0x02,0x00, 0x0,+0 }, // 1062: oGM15; Dulcimer
    { 0x000A121,0x0F6F236, 0x80,0x00, 0x8,+0 }, // 1063: f15GM16; f15GM17; f15GM18; f26GM16; f26GM17; f26GM18; oGM16; oGM17; oGM18; Hammond Organ; Percussive Organ; Rock Organ
    { 0x085F211,0x0B7F212, 0x87,0x80, 0x4,+0 }, // 1064: f15GM19; f15GM20; f15GM21; f26GM19; f26GM20; f26GM21; oGM19; oGM20; oGM21; Accordion; Church Organ; Reed Organ
    { 0x054F607,0x0B6F242, 0x73,0x00, 0x0,+0 }, // 1065: f15GM22; f26GM22; oGM22; Harmonica
    { 0x054F60E,0x0B6F242, 0x73,0x00, 0x0,+0 }, // 1066: f15GM23; f26GM23; oGM23; Tango Accordion
    { 0x1E26301,0x01EB821, 0x16,0x00, 0x8,+0 }, // 1067: oGM24; Acoustic Guitar1
    { 0x1226341,0x01E8821, 0x8F,0x00, 0x8,+0 }, // 1068: oGM25; Acoustic Guitar2
    { 0x0024471,0x01E8831, 0x9D,0x00, 0xE,+0 }, // 1069: oGM27; Electric Guitar2
    { 0x002A434,0x0427575, 0x54,0x40, 0x8,+0 }, // 1070: oGM32; Acoustic Bass
    { 0x00211B1,0x0034231, 0x93,0x80, 0x0,+0 }, // 1071: f15GM33; f26GM33; oGM33; Electric Bass 1
    { 0x0023AB1,0x0134232, 0xAF,0x80, 0x0,+0 }, // 1072: f15GM34; f26GM34; oGM34; Electric Bass 2
    { 0x256F605,0x2047404, 0xC0,0x00, 0xE,+0 }, // 1073: oGM35; Fretless Bass
    { 0x05312C4,0x07212F1, 0x10,0x00, 0x2,+0 }, // 1074: f15GM36; f26GM36; oGM36; Slap Bass 1
    { 0x061F217,0x074F212, 0x4F,0x00, 0x8,+0 }, // 1075: f15GM38; f26GM38; oGM38; Synth Bass 1
    { 0x02FA433,0x0117575, 0x14,0x00, 0x0,+0 }, // 1076: f15GM40; f26GM40; oGM40; Violin
    { 0x0FFF09E,0x00F3F00, 0x07,0x00, 0xE,+0 }, // 1077: oGM41; Viola
    { 0x0124D01,0x013F501, 0x02,0x00, 0x7,+0 }, // 1078: f15GM43; f26GM43; oGM43; Contrabass
    { 0x20FFF22,0x00FFF21, 0x5A,0x80, 0x0,+0 }, // 1079: f15GM47; f26GM47; oGM47; Timpany
    { 0x1217131,0x0066222, 0x40,0x40, 0x2,+0 }, // 1080: oGM48; String Ensemble1
    { 0x121F131,0x0166F21, 0x40,0x00, 0x2,+0 }, // 1081: f15GM49; f26GM49; oGM49; String Ensemble2
    { 0x131F231,0x0066F21, 0x47,0x00, 0x0,+0 }, // 1082: oGM50; Synth Strings 1
    { 0x175F502,0x0F8F501, 0x58,0x80, 0x0,+0 }, // 1083: f15GM51; f26GM51; oGM51; SynthStrings 2
    { 0x0035131,0x06764A1, 0x1C,0x80, 0xE,+0 }, // 1084: oGM53; Voice Oohs
    { 0x0115270,0x0FE4171, 0xC5,0x40, 0x0,+0 }, // 1085: oGM55; Orchestra Hit
    { 0x1218131,0x0167423, 0x4D,0x40, 0x2,+0 }, // 1086: oGM56; Trumpet
    { 0x151D203,0x278F301, 0x1D,0x00, 0xA,+0 }, // 1087: oGM59; Muted Trumpet
    { 0x0F0F09E,0x063F300, 0x07,0x00, 0xE,+0 }, // 1088: oGM60; French Horn
    { 0x0F7B096,0x00FFFE0, 0x00,0x00, 0x0,+0 }, // 1089: oGM61; Brass Section
    { 0x3199B85,0x0297424, 0x49,0x00, 0x6,+0 }, // 1090: oGM62; Synth Brass 1
    { 0x0FFA691,0x0F45511, 0x00,0x00, 0x8,+0 }, // 1091: oGM63; Synth Brass 2
    { 0x05FF561,0x02AF562, 0x21,0x00, 0x2,+0 }, // 1092: f15GM64; f26GM64; oGM64; Soprano Sax
    { 0x04F6421,0x028F231, 0x91,0x00, 0xA,+0 }, // 1093: f15GM70; f15GM71; f26GM70; f26GM71; oGM70; oGM71; Bassoon; Clarinet
    { 0x05FF561,0x05A6661, 0x1E,0x00, 0x2,+0 }, // 1094: f15GM72; f26GM72; oGM72; Piccolo
    { 0x05FF561,0x02A7561, 0x1E,0x07, 0x2,+0 }, // 1095: f15GM73; f15GM74; f26GM73; f26GM74; oGM73; oGM74; Flute; Recorder
    { 0x03FF561,0x01A7562, 0x28,0x04, 0x2,+0 }, // 1096: f15GM75; f26GM75; oGM75; Pan Flute
    { 0x01F7561,0x02A7561, 0x21,0x00, 0x2,+0 }, // 1097: f15GM76; f26GM76; oGM76; Bottle Blow
    { 0x1226341,0x000A821, 0x8F,0x00, 0x8,+0 }, // 1098: oGM78; Whistle
    { 0x1239721,0x0136572, 0x8A,0x80, 0x2,+0 }, // 1099: oGM79; Ocarina
    { 0x061F217,0x074F212, 0x6C,0x00, 0x8,+0 }, // 1100: oGM80; Lead 1 squareea
    { 0x1239721,0x0138572, 0x8A,0x80, 0x2,+0 }, // 1101: oGM81; Lead 2 sawtooth
    { 0x0217B32,0x0176221, 0x95,0x00, 0x0,+0 }, // 1102: f15GM82; f26GM82; oGM82; Lead 3 calliope
    { 0x0219B32,0x0176221, 0x97,0x00, 0x0,+0 }, // 1103: f15GM83; f26GM83; oGM83; Lead 4 chiff
    { 0x0115231,0x11E3132, 0xC5,0x00, 0x8,+0 }, // 1104: f15GM84; f26GM84; oGM84; Lead 5 charang
    { 0x1177E31,0x10C8B21, 0x43,0x00, 0x2,+0 }, // 1105: f15GM85; f26GM85; oGM85; Lead 6 voice
    { 0x019D520,0x11B6121, 0x93,0x00, 0xC,+0 }, // 1106: f15GM86; f26GM86; oGM86; Lead 7 fifths
    { 0x00D5131,0x01F7221, 0x1C,0x80, 0xE,+0 }, // 1107: f26GM88; oGM88; oGM89; Pad 1 new age; Pad 2 warm
    { 0x00D5131,0x01F7221, 0x1C,0x80, 0xC,+0 }, // 1108: oGM90; oGM91; Pad 3 polysynth; Pad 4 choir
    { 0x06A6121,0x00A7F21, 0x26,0x00, 0x2,+0 }, // 1109: f26GM92; f26GM93; oGM92; oGM93; Pad 5 bowedpad; Pad 6 metallic
    { 0x08C6320,0x02F9520, 0x19,0x80, 0xC,+0 }, // 1110: oGM94; Pad 7 halo
    { 0x033F5C5,0x025FDE1, 0x53,0x80, 0xA,+0 }, // 1111: f15GM97; f26GM97; oGM97; oGM98; FX 2 soundtrack; FX 3 crystal
    { 0x05FF561,0x01A6661, 0x1E,0x00, 0x2,+0 }, // 1112: f15GM107; f26GM107; oGM107; Koto
    { 0x1F5E510,0x162E231, 0x46,0x00, 0x0,+0 }, // 1113: oGM112; Tinkle Bell
    { 0x24FF60E,0x318F700, 0x40,0x00, 0xE,+0 }, // 1114: oGM114; oGP38; oGP40; Acoustic Snare; Electric Snare; Steel Drums
    { 0x0C8F60C,0x257FF12, 0xC0,0x00, 0xA,+0 }, // 1115: oGM115; oGP42; Closed High Hat; Woodblock
    { 0x354B506,0x095D507, 0x00,0xC0, 0x0,+0 }, // 1116: oGM116; oGM119; oGP51; Reverse Cymbal; Ride Cymbal 1; Taiko Drum
    { 0x04CA800,0x13FD600, 0x0B,0x00, 0x0,+0 }, // 1117: oGM117; oGM120; oGP37; oGP39; oGP41; oGP43; oGP45; oGP47; oGP48; oGP50; Guitar FretNoise; Hand Clap; High Floor Tom; High Tom; High-Mid Tom; Low Floor Tom; Low Tom; Low-Mid Tom; Melodic Tom; Side Stick
    { 0x0F0E02A,0x031FF1E, 0x52,0x54, 0x8,+0 }, // 1118: oGM121; Breath Noise
    { 0x0745451,0x0756591, 0x00,0x00, 0xA,+0 }, // 1119: oGM122; Seashore
    { 0x002A414,0x0427555, 0x54,0x40, 0x8,+0 }, // 1120: oGM124; Telephone
    { 0x0115F31,0x11E3132, 0xC5,0x00, 0x8,+0 }, // 1121: oGM125; Helicopter
    { 0x1217131,0x0069222, 0x40,0x40, 0x2,+0 }, // 1122: oGM126; Applause/Noise
    { 0x000F60E,0x3059F10, 0x00,0x00, 0xE,+0 }, // 1123: f15GP44; f26GP44; oGP44; Pedal High Hat
    { 0x000F60E,0x3039F10, 0x00,0x00, 0xE,+0 }, // 1124: f15GP46; f26GP46; oGP46; Open High Hat
    { 0x0C5F59E,0x2F7F70E, 0x00,0x00, 0xF,+0 }, // 1125: f15GP54; f26GP54; oGP54; Tambourine
    { 0x0BFFA01,0x097C803, 0x00,0x00, 0x7,+0 }, // 1126: f15GP60; f26GP60; oGP60; High Bongo
    { 0x053F101,0x0B5F704, 0x4F,0x00, 0x7,+0 }, // 1127: oGP62; oGP63; oGP64; Low Conga; Mute High Conga; Open High Conga
    { 0x04FF60E,0x218F700, 0x40,0x00, 0xE,+0 }, // 1128: oGP69; oGP70; Cabasa; Maracas
    { 0x0F4F306,0x0E4E203, 0xA4,0x6D, 0x6,+0 }, // 1129: f12GM0; f16GM0; f54GM0; AcouGrandPiano
    { 0x0D4E101,0x0E5E111, 0x53,0x02, 0x6,+0 }, // 1130: f12GM0; f16GM0; f54GM0; AcouGrandPiano
    { 0x053F241,0x0F3F213, 0x9D,0x00, 0x6,+0 }, // 1131: f12GM1; f16GM1; f54GM1; BrightAcouGrand
    { 0x050F101,0x076D201, 0x4F,0x04, 0x6,+0 }, // 1132: f12GM2; f16GM2; f54GM2; ElecGrandPiano
    { 0x053F101,0x0849212, 0xC3,0x09, 0x8,+0 }, // 1133: f12GM3; f16GM3; f54GM3; Honky-tonkPiano
    { 0x074F202,0x077F401, 0x92,0x83, 0x8,+0 }, // 1134: f12GM4; f16GM4; f54GM4; Rhodes Piano
    { 0x013F202,0x044F502, 0x22,0x00, 0xE,+0 }, // 1135: f12GM5; f16GM5; f54GM5; Chorused Piano
    { 0x475F113,0x256F201, 0x96,0x81, 0x6,+0 }, // 1136: f12GM6; f16GM6; f54GM6; Harpsichord
    { 0x0100133,0x033AD14, 0x87,0x80, 0x8,+0 }, // 1137: f12GM8; f16GM8; f54GM8; Celesta
    { 0x0E5F14C,0x0E5C301, 0x69,0x06, 0x8,+0 }, // 1138: f12GM9; f16GM9; f54GM9; Glockenspiel
    { 0x0E2660F,0x0E4C191, 0x9D,0x06, 0xE,+0 }, // 1139: f12GM10; f16GM10; f54GM10; Music box
    { 0x033F584,0x015FDA0, 0x59,0x80, 0x2,+0 }, // 1140: f12GM11; f16GM11; f54GM11; Vibraphone
    { 0x0B5F615,0x0E6F311, 0x97,0x01, 0x4,+0 }, // 1141: f12GM12; f16GM12; f54GM12; Marimba
    { 0x0F8FF06,0x055F8C4, 0x01,0x00, 0xE,+0 }, // 1142: f12GM13; f16GM13; f54GM13; Xylophone
    { 0x063F207,0x074F212, 0x4F,0x00, 0x8,+0 }, // 1143: f12GM14; f16GM14; f53GM122; f54GM14; Seashore; Tubular Bells
    { 0x341F5A3,0x203F811, 0x11,0x00, 0x0,+0 }, // 1144: f12GM15; f16GM15; f54GM15; Dulcimer
    { 0x01AF003,0x01DF001, 0x5B,0x80, 0xA,+0 }, // 1145: f12GM16; f16GM16; f54GM16; Hammond Organ
    { 0x22A9132,0x12A91B1, 0xCD,0x80, 0x9,+0 }, // 1146: f12GM17; f16GM17; f54GM17; Percussive Organ
    { 0x0038165,0x005F171, 0xD2,0x80, 0x2,+0 }, // 1147: f12GM18; f16GM18; f54GM18; Rock Organ
    { 0x00AFF24,0x00DFF21, 0x80,0x80, 0x1,+0 }, // 1148: f12GM19; f16GM19; f54GM19; Church Organ
    { 0x01CF003,0x01EA001, 0x54,0x84, 0xC,+0 }, // 1149: f12GM20; f16GM20; f54GM20; Reed Organ
    { 0x0186223,0x02A6221, 0x19,0x84, 0xE,+0 }, // 1150: f12GM21; f16GM21; f54GM21; Accordion
    { 0x0087224,0x00B4231, 0x4F,0x00, 0xE,+0 }, // 1151: f12GM22; f16GM22; f54GM22; Harmonica
    { 0x0186222,0x02A6221, 0x19,0x84, 0xE,+0 }, // 1152: f12GM23; f16GM23; f54GM23; Tango Accordion
    { 0x074A302,0x075C441, 0x9A,0x80, 0xA,+0 }, // 1153: f12GM24; f16GM24; f54GM24; Acoustic Guitar1
    { 0x0C3C201,0x056F501, 0x0A,0x00, 0x6,+0 }, // 1154: f12GM25; f16GM25; f54GM25; Acoustic Guitar2
    { 0x034F401,0x039F201, 0x13,0x80, 0x8,+0 }, // 1155: f12GM26; f16GM26; f54GM26; Electric Guitar1
    { 0x07FC611,0x0DFF511, 0x4D,0x00, 0x6,+0 }, // 1156: f12GM28; f16GM28; f54GM28; Electric Guitar3
    { 0x4C5A421,0x004F821, 0x20,0x00, 0x2,+0 }, // 1157: f12GM32; f16GM32; f54GM32; Acoustic Bass
    { 0x0E78301,0x078F201, 0x56,0x00, 0xA,+0 }, // 1158: f12GM33; f16GM33; f54GM33; Electric Bass 1
    { 0x0AFF301,0x078F501, 0x11,0x00, 0x8,+0 }, // 1159: f12GM34; f16GM34; f54GM34; Electric Bass 2
    { 0x114FF20,0x0D4F561, 0xCB,0x00, 0xC,+0 }, // 1160: f12GM35; f16GM35; f54GM35; Fretless Bass
    { 0x1937510,0x182F501, 0x00,0x00, 0x0,+0 }, // 1161: f12GM36; f12GM37; f16GM36; f16GM37; f54GM36; Slap Bass 1; Slap Bass 2
    { 0x01379C0,0x07472D2, 0x4F,0x00, 0x6,+12 }, // 1162: f12GM38; f16GM38; f54GM38; Synth Bass 1
    { 0x2355612,0x12D9531, 0x9C,0x00, 0xA,+0 }, // 1163: f12GM39; f16GM39; f54GM39; Synth Bass 2
    { 0x0035131,0x0675461, 0x1C,0x80, 0xE,+0 }, // 1164: b41M53; f12GM40; f16GM40; f32GM53; f41GM53; f54GM40; Violin; Voice Oohs; violin1.
    { 0x21351A0,0x2275360, 0x9B,0x01, 0xE,+0 }, // 1165: f12GM41; f47GM41; Viola
    { 0x163F2A1,0x0368331, 0x48,0x00, 0x6,+0 }, // 1166: f12GM42; f16GM42; f54GM42; Cello
    { 0x171A501,0x2539600, 0x0D,0x02, 0x7,+0 }, // 1167: f12GM42; f16GM42; f54GM42; Cello
    { 0x051F431,0x074B711, 0x57,0x00, 0xC,+0 }, // 1168: f12GM45; f16GM45; f54GM45; Pizzicato String
    { 0x005F624,0x095C702, 0xDB,0x23, 0x8,+0 }, // 1169: f12GM46; Orchestral Harp
    { 0x095F422,0x0D5F401, 0x22,0x00, 0x8,+0 }, // 1170: f12GM46; Orchestral Harp
    { 0x7D2FE85,0x074F342, 0x8F,0x80, 0x6,-12 }, // 1171: f12GM116; f12GM47; f16GM116; f16GM47; f37GM47; f54GM116; f54GM47; Taiko Drum; Timpany
    { 0x016F521,0x03493A1, 0x8C,0x00, 0x0,+0 }, // 1172: f12GM48; String Ensemble1
    { 0x0013121,0x10545A1, 0x4D,0x82, 0x6,+0 }, // 1173: f12GM49; f16GM49; f37GM49; f54GM49; String Ensemble2
    { 0x01FB431,0x01FA2A1, 0x1A,0x80, 0xE,+0 }, // 1174: f12GM56; Trumpet
    { 0x04654A1,0x0078FA1, 0x1C,0x07, 0xE,+0 }, // 1175: f12GM57; f16GM57; f54GM57; Trombone
    { 0x0466421,0x0078FE1, 0x14,0x01, 0xF,+0 }, // 1176: f12GM57; f16GM57; f54GM57; Trombone
    { 0x0796520,0x0268AA1, 0x8C,0x03, 0x8,+12 }, // 1177: f12GM58; Tuba
    { 0x2179280,0x03686A0, 0xCF,0x00, 0x9,+0 }, // 1178: f12GM58; Tuba
    { 0x21A73A0,0x03A8523, 0x95,0x00, 0xE,+0 }, // 1179: f12GM59; f16GM59; f37GM59; f54GM59; Muted Trumpet
    { 0x03A5321,0x00B6521, 0x9C,0x01, 0xA,+0 }, // 1180: f12GM60; French Horn
    { 0x01C7321,0x02C7C21, 0xC0,0x97, 0xB,+0 }, // 1181: f12GM60; French Horn
    { 0x06581E1,0x07C52F2, 0x51,0x00, 0xC,+0 }, // 1182: f12GM64; f16GM64; f54GM64; Soprano Sax
    { 0x2197320,0x0297563, 0x22,0x02, 0xE,+0 }, // 1183: f12GM68; f16GM68; f47GM69; f54GM68; English Horn; Oboe
    { 0x22E71E0,0x01E80E4, 0x23,0x00, 0xA,+0 }, // 1184: f12GM69; f16GM69; f54GM69; English Horn
    { 0x019D530,0x01B6171, 0xC8,0x80, 0xC,+0 }, // 1185: f12GM70; f16GM70; f47GM70; f54GM70; Bassoon
    { 0x01582A3,0x007E562, 0x21,0x9E, 0xE,+0 }, // 1186: f12GM71; f16GM71; f54GM71; Clarinet
    { 0x005D224,0x0076F21, 0x9F,0x02, 0xF,+0 }, // 1187: f12GM71; f16GM71; f54GM71; Clarinet
    { 0x48674A1,0x02765A1, 0x1F,0x00, 0x0,+0 }, // 1188: f12GM72; f16GM72; f54GM72; Piccolo
    { 0x08F74A1,0x02A65A1, 0x27,0x80, 0x2,+0 }, // 1189: f12GM73; f16GM73; f32GM72; f32GM73; f32GM74; f32GM75; f37GM72; f37GM73; f47GM73; f54GM73; Flute; Pan Flute; Piccolo; Recorder
    { 0x0277584,0x01655A1, 0xA0,0x81, 0xC,+0 }, // 1190: f12GM74; f16GM74; f54GM74; Recorder
    { 0x01566A2,0x00566A1, 0x8A,0x00, 0xD,+0 }, // 1191: f12GM74; f16GM74; f54GM74; Recorder
    { 0x016D322,0x07DE82F, 0x9B,0x2E, 0xE,-12 }, // 1192: f12GM77; f16GM77; f53GM77; f54GM77; Shakuhachi
    { 0x006C524,0x02764B2, 0x62,0x04, 0xE,+0 }, // 1193: f12GM77; f16GM77; f53GM77; f54GM77; Shakuhachi
    { 0x0557221,0x096F481, 0x0B,0x08, 0x6,+0 }, // 1194: f12GM84; f16GM84; f54GM84; Lead 5 charang
    { 0x0A6CF22,0x09C8410, 0xD5,0x0D, 0x7,+0 }, // 1195: f12GM84; f16GM84; f54GM84; Lead 5 charang
    { 0x001F501,0x0F1F101, 0x37,0x20, 0x0,+0 }, // 1196: f12GM106; f16GM106; f54GM106; Shamisen
    { 0x0E3F201,0x0E7F501, 0x11,0x00, 0x0,+0 }, // 1197: f12GM106; f16GM106; f54GM106; Shamisen
    { 0x03CF201,0x0E2F111, 0x3F,0x14, 0x0,+0 }, // 1198: f12GM107; f16GM107; f53GM106; f54GM107; Koto; Shamisen
    { 0x0E6F541,0x0E7F312, 0x13,0x01, 0x0,+0 }, // 1199: f12GM107; f16GM107; f53GM106; f54GM107; Koto; Shamisen
    { 0x01582A3,0x00AF562, 0x21,0xA3, 0xE,+0 }, // 1200: f12GM111; f16GM111; f53GM82; f54GM111; Lead 3 calliope; Shanai
    { 0x005F224,0x00A6F21, 0xA2,0x09, 0xF,+0 }, // 1201: f12GM111; f16GM111; f53GM82; f54GM111; Lead 3 calliope; Shanai
    { 0x0FFF832,0x07FF511, 0x44,0x00, 0xE,+0 }, // 1202: f12GM115; f16GM115; f47GP10; f47GP11; f47GP12; f47GP13; f47GP14; f47GP15; f47GP16; f47GP17; f47GP18; f47GP19; f47GP20; f47GP21; f47GP22; f47GP23; f47GP24; f47GP25; f47GP26; f47GP27; f47GP62; f47GP63; f47GP64; f47GP7; f47GP8; f47GP9; f54GM115; Low Conga; Mute High Conga; Open High Conga; Woodblock
    { 0x137FB00,0x05CE711, 0x05,0x00, 0x8,+0 }, // 1203: f12GP31; f12GP36; f16GP31; f16GP36; f37GP31; f37GP36; f54GP31; f54GP36; f54GP60; f54GP61; f54GP62; f54GP63; f54GP64; f54GP65; f54GP66; Bass Drum 1; High Bongo; High Timbale; Low Bongo; Low Conga; Low Timbale; Mute High Conga; Open High Conga
    { 0x0F0F006,0x2B6F800, 0x00,0x00, 0xE,+0 }, // 1204: f12GP33; f16GP33; f54GP33
    { 0x04CA900,0x03FF600, 0x07,0x00, 0xA,+0 }, // 1205: f12GP35; f54GP35; Ac Bass Drum
    { 0x008B902,0x01DFC03, 0x00,0x00, 0xB,+0 }, // 1206: f12GP35; f54GP35; Ac Bass Drum
    { 0x60AF905,0x41CFC0A, 0x00,0x00, 0xD,+0 }, // 1207: f12GP37; f16GP37; f54GP37; Side Stick
    { 0x033F400,0x4FFF700, 0x04,0x00, 0xE,+0 }, // 1208: f12GP38; f16GP38; f54GP38; Acoustic Snare
    { 0x40AFF02,0x01CFF00, 0xC0,0x01, 0x4,+0 }, // 1209: f12GP39; f16GP39; f54GP39; Hand Clap
    { 0x023F302,0x067F700, 0x08,0x00, 0xE,+0 }, // 1210: f12GP40; f16GP40; f37GP40; f54GP40; Electric Snare
    { 0x017FB01,0x008FD02, 0x40,0x00, 0x9,+0 }, // 1211: f12GP41; f12GP43; f12GP45; f12GP47; f12GP48; f12GP50; f16GP41; f16GP43; f16GP45; f16GP47; f16GP48; f16GP50; f37GP41; f37GP43; f37GP45; f37GP47; f54GP41; f54GP43; f54GP45; f54GP47; f54GP48; f54GP50; High Floor Tom; High Tom; High-Mid Tom; Low Floor Tom; Low Tom; Low-Mid Tom
    { 0x003F902,0x247FB00, 0x00,0x00, 0xE,+0 }, // 1212: f12GP42; f54GP42; Closed High Hat
    { 0x403FB02,0x447FB01, 0x00,0x00, 0xE,+0 }, // 1213: f12GP42; f54GP42; Closed High Hat
    { 0x609F505,0x709F30F, 0x00,0x00, 0x6,+0 }, // 1214: f12GP44; f12GP46; f12GP54; f16GP42; f16GP44; f16GP46; f16GP54; f54GP44; f54GP46; f54GP54; Closed High Hat; Open High Hat; Pedal High Hat; Tambourine
    { 0x201C687,0x023BC15, 0xC0,0x40, 0xE,+0 }, // 1215: f12GP49; f16GP49; f54GP49; Crash Cymbal 1
    { 0x435DE00,0x438F801, 0xC0,0x00, 0xA,+0 }, // 1216: f12GP56; f16GP56; f54GP56; Cow Bell
    { 0x30AF400,0x278F700, 0x47,0x04, 0xE,+0 }, // 1217: f12GP60; High Bongo
    { 0x30AF400,0x278F700, 0x4B,0x02, 0xE,+0 }, // 1218: f12GP61; Low Bongo
    { 0x509F601,0x429F701, 0x00,0x00, 0x7,+0 }, // 1219: f12GP60; f12GP61; f16GP60; f16GP61; High Bongo; Low Bongo
    { 0x407FF00,0x769A901, 0x00,0x40, 0x9,+0 }, // 1220: f12GP63; f16GP63; f16GP64; Low Conga; Open High Conga
    { 0x408FA01,0x769DB02, 0x00,0x40, 0x7,+0 }, // 1221: f12GP64; Low Conga
    { 0x104F021,0x0D6F401, 0xCF,0x00, 0xA,+0 }, // 1222: f13GM0; f50GM0; AcouGrandPiano
    { 0x104F021,0x0D6F401, 0xC7,0x00, 0x0,+0 }, // 1223: f13GM1; f50GM1; BrightAcouGrand
    { 0x004F021,0x0D6F401, 0x1B,0x00, 0xA,+0 }, // 1224: f13GM2; f50GM2; ElecGrandPiano
    { 0x104F0A1,0x1D6F481, 0xCE,0x00, 0x4,+0 }, // 1225: f13GM3; f50GM3; Honky-tonkPiano
    { 0x065F301,0x07DF111, 0x12,0x00, 0x8,+0 }, // 1226: f13GM4; f50GM4; Rhodes Piano
    { 0x254F568,0x0B7F321, 0xE8,0x00, 0x0,+0 }, // 1227: f13GM5; f50GM5; Chorused Piano
    { 0x14FF101,0x3D6F311, 0xC6,0x06, 0xA,+0 }, // 1228: f13GM6; f50GM6; Harpsichord
    { 0x0ADF303,0x15E8301, 0x58,0x00, 0xE,+0 }, // 1229: f13GM7; f50GM7; Clavinet
    { 0x01F4C28,0x045F601, 0xD4,0x00, 0xE,+0 }, // 1230: f13GM8; f50GM8; Celesta
    { 0x223F208,0x073F414, 0x92,0x80, 0x0,-12 }, // 1231: f13GM9; f50GM9; Glockenspiel
    { 0x22F6216,0x06AF401, 0x64,0x41, 0x0,+0 }, // 1232: f13GM10; f50GM10; Music box
    { 0x036F506,0x025FD61, 0x10,0x80, 0x3,+0 }, // 1233: f13GM11; f50GM11; Vibraphone
    { 0x0176D0A,0x005F001, 0xD5,0x00, 0x4,+0 }, // 1234: f13GM12; f50GM12; Marimba
    { 0x265F812,0x0D7F601, 0xC8,0x00, 0xC,+0 }, // 1235: f13GM13; f50GM13; Xylophone
    { 0x092FF83,0x003F015, 0x00,0x00, 0xE,-12 }, // 1236: f13GM14; f50GM14; Tubular Bells
    { 0x0388B03,0x2398300, 0xC0,0x80, 0x0,+0 }, // 1237: f13GM15; f50GM15; Dulcimer
    { 0x00FF0A0,0x00FF0A2, 0xC0,0x06, 0xD,+0 }, // 1238: f13GM16; f50GM16; Hammond Organ
    { 0x29FFF24,0x10FF021, 0x00,0x00, 0xF,+0 }, // 1239: f13GM17; f50GM17; Percussive Organ
    { 0x11FFF30,0x14C5E32, 0x00,0x00, 0x7,+0 }, // 1240: f13GM18; f50GM18; Rock Organ
    { 0x10BF024,0x20B5030, 0x12,0x00, 0x1,+0 }, // 1241: f13GM19; f50GM19; Church Organ
    { 0x10BF024,0x20B5030, 0x49,0x00, 0xF,+0 }, // 1242: f13GM20; f50GM20; Reed Organ
    { 0x00BF024,0x10B5031, 0xCC,0x0A, 0xA,+0 }, // 1243: f13GM21; f50GM21; Accordion
    { 0x12F6F24,0x20D4030, 0xCA,0x0A, 0x0,+0 }, // 1244: f13GM22; f50GM22; Harmonica
    { 0x00BF022,0x10B50B1, 0xCD,0x03, 0x0,+0 }, // 1245: f13GM23; f50GM23; Tango Accordion
    { 0x105F003,0x1C8F211, 0xCE,0x00, 0x0,+0 }, // 1246: f13GM24; f50GM24; Acoustic Guitar1
    { 0x125FF03,0x1C8F211, 0x49,0x00, 0x0,+0 }, // 1247: f13GM25; f50GM25; Acoustic Guitar2
    { 0x145F503,0x03AF621, 0xD3,0x00, 0xE,+0 }, // 1248: f13GM26; f50GM26; Electric Guitar1
    { 0x1269E03,0x0BBF221, 0x90,0x80, 0xE,+0 }, // 1249: f13GM27; f50GM27; Electric Guitar2
    { 0x047FF01,0x2BCF400, 0xC0,0x00, 0xE,+0 }, // 1250: f13GM28; f50GM28; Electric Guitar3
    { 0x04F6F20,0x31FFF20, 0xE0,0x01, 0x0,+0 }, // 1251: f13GM29; f50GM29; Overdrive Guitar
    { 0x32F5F30,0x31FFE30, 0xE0,0x01, 0x0,+0 }, // 1252: f13GM30; f50GM30; Distorton Guitar
    { 0x3598600,0x02A7244, 0x42,0x80, 0xC,+0 }, // 1253: f13GM31; f50GM31; Guitar Harmonics
    { 0x054FE10,0x00FF030, 0x00,0x00, 0x6,+12 }, // 1254: f13GM32; f50GM32; Acoustic Bass
    { 0x0397530,0x088F220, 0xC2,0x40, 0x8,+12 }, // 1255: f13GM33; f50GM33; Electric Bass 1
    { 0x125FF10,0x006F030, 0x0A,0x00, 0xC,+12 }, // 1256: f13GM34; f50GM34; Electric Bass 2
    { 0x039F330,0x00CF060, 0x0F,0x00, 0x8,+12 }, // 1257: f13GM35; f50GM35; Fretless Bass
    { 0x07FF420,0x00FF021, 0x18,0x00, 0xE,+0 }, // 1258: f13GM36; f50GM36; Slap Bass 1
    { 0x106F010,0x006F030, 0x00,0x00, 0x6,+12 }, // 1259: f13GM37; f50GM37; Slap Bass 2
    { 0x05FF620,0x00FF021, 0x16,0x00, 0xE,+0 }, // 1260: f13GM38; f50GM38; Synth Bass 1
    { 0x006F010,0x006F030, 0x08,0x00, 0x4,+0 }, // 1261: f13GM39; f50GM39; Synth Bass 2
    { 0x1378D31,0x0163871, 0x85,0x00, 0xA,+0 }, // 1262: f13GM40; Violin
    { 0x106F031,0x1065071, 0xC5,0x00, 0x0,+0 }, // 1263: f13GM41; f50GM41; Viola
    { 0x11FF431,0x1365361, 0x40,0x00, 0x0,+0 }, // 1264: f13GM42; f50GM42; Cello
    { 0x01FF431,0x1366361, 0xC0,0x00, 0x0,+0 }, // 1265: f13GM43; f50GM43; Contrabass
    { 0x043F2B1,0x12851A1, 0x1D,0x00, 0xE,+0 }, // 1266: f13GM44; f50GM44; Tremulo Strings
    { 0x279A702,0x284F410, 0xD2,0x00, 0x0,+0 }, // 1267: f13GM45; f50GM45; Pizzicato String
    { 0x194F622,0x09BF231, 0x1B,0x80, 0xA,+0 }, // 1268: f13GM46; f50GM46; Orchestral Harp
    { 0x126F801,0x105F000, 0x40,0x00, 0x0,+0 }, // 1269: f13GM47; f50GM47; Timpany
    { 0x043F231,0x1285121, 0x1D,0x00, 0xE,+0 }, // 1270: f13GM48; f50GM48; String Ensemble1
    { 0x1011031,0x2042030, 0x56,0x00, 0xE,+0 }, // 1271: f13GM49; f50GM49; String Ensemble2
    { 0x136F131,0x0286121, 0x1B,0x00, 0xE,+0 }, // 1272: f13GM50; f50GM50; Synth Strings 1
    { 0x034F131,0x0285121, 0x1C,0x00, 0xE,+0 }, // 1273: f13GM51; f50GM51; SynthStrings 2
    { 0x015F431,0x00560B2, 0x5B,0x83, 0x0,+0 }, // 1274: f13GM52; f50GM52; Choir Aahs
    { 0x172FCE1,0x0176271, 0x46,0x00, 0x0,+0 }, // 1275: f13GM53; f50GM53; Voice Oohs
    { 0x00530B1,0x00550B2, 0x57,0x00, 0xC,+0 }, // 1276: f13GM54; f50GM54; Synth Voice
    { 0x062F600,0x01BF301, 0x00,0x08, 0x6,+0 }, // 1277: f13GM55; f50GM55; Orchestra Hit
    { 0x0655371,0x00FF021, 0x14,0x00, 0xA,+0 }, // 1278: f13GM56; f50GM56; Trumpet
    { 0x0254231,0x00FF061, 0x56,0x01, 0xE,+0 }, // 1279: f13GM57; f50GM57; Trombone
    { 0x1255221,0x0299361, 0x55,0x01, 0xE,+0 }, // 1280: f13GM58; f50GM58; Tuba
    { 0x0755471,0x0089021, 0x20,0x00, 0xE,+0 }, // 1281: f13GM59; f50GM59; Muted Trumpet
    { 0x0265121,0x007F021, 0x18,0x00, 0xA,+0 }, // 1282: f13GM60; f50GM60; French Horn
    { 0x0375421,0x008F021, 0x1B,0x00, 0xE,+0 }, // 1283: f13GM61; f50GM61; Brass Section
    { 0x1396521,0x09EF221, 0x16,0x00, 0xE,+0 }, // 1284: f13GM62; f50GM62; Synth Brass 1
    { 0x0375621,0x00AF021, 0x1E,0x00, 0xE,+0 }, // 1285: f13GM63; f50GM63; Synth Brass 2
    { 0x0046021,0x1095031, 0x4E,0x00, 0x6,+0 }, // 1286: f13GM64; f50GM64; Soprano Sax
    { 0x0046021,0x1095031, 0x8E,0x00, 0xA,+0 }, // 1287: f13GM65; f50GM65; Alto Sax
    { 0x0055021,0x1095021, 0x8E,0x00, 0xA,+0 }, // 1288: f13GM66; f50GM66; Tenor Sax
    { 0x0055031,0x1095021, 0x8E,0x00, 0xA,+0 }, // 1289: f13GM67; f50GM67; Baritone Sax
    { 0x0038031,0x136F132, 0x17,0x00, 0x0,+0 }, // 1290: f13GM68; f50GM68; Oboe
    { 0x2066020,0x10A7022, 0x19,0x00, 0x0,+0 }, // 1291: f13GM69; f50GM69; English Horn
    { 0x1065020,0x00A6022, 0x1E,0x00, 0x0,+0 }, // 1292: f13GM70; f50GM70; Bassoon
    { 0x0258C32,0x0176221, 0x4C,0x00, 0xC,+0 }, // 1293: f13GM71; f50GM71; Clarinet
    { 0x0043071,0x00A5021, 0x57,0x00, 0xC,+0 }, // 1294: f13GM72; f50GM72; Piccolo
    { 0x0445171,0x00A5021, 0x55,0x00, 0xC,+0 }, // 1295: f13GM73; f50GM73; Flute
    { 0x20F4032,0x0095021, 0xDF,0x00, 0x0,+0 }, // 1296: f13GM74; f50GM74; Recorder
    { 0x39C4611,0x05A6321, 0x20,0x00, 0xE,+0 }, // 1297: f13GM75; f50GM75; Pan Flute
    { 0x39D7531,0x0095021, 0x17,0x00, 0xE,+0 }, // 1298: f13GM76; f50GM76; Bottle Blow
    { 0x35AF802,0x02A4271, 0x00,0x00, 0xE,+0 }, // 1299: f13GM77; f50GM77; Shakuhachi
    { 0x08F4EE0,0x02A55A1, 0xEC,0x00, 0xE,+0 }, // 1300: f13GM78; Whistle
    { 0x00F4032,0x0097021, 0xDF,0x00, 0x0,+0 }, // 1301: f13GM79; f50GM79; Ocarina
    { 0x20FF022,0x00FF021, 0x5D,0x00, 0xE,+0 }, // 1302: f13GM80; f50GM80; Lead 1 squareea
    { 0x0535231,0x147F221, 0x0F,0x00, 0xC,+0 }, // 1303: f13GM81; f50GM81; Lead 2 sawtooth
    { 0x39D6571,0x0095021, 0x17,0x00, 0xE,+0 }, // 1304: f13GM82; f50GM82; Lead 3 calliope
    { 0x05AF802,0x22A4270, 0x00,0x00, 0xE,+0 }, // 1305: f13GM83; f50GM83; Lead 4 chiff
    { 0x057F421,0x228F232, 0xC0,0x00, 0x0,+0 }, // 1306: f13GM84; f50GM84; Lead 5 charang
    { 0x29D65A1,0x2095021, 0xC6,0x00, 0x0,-12 }, // 1307: f13GM85; f50GM85; Lead 6 voice
    { 0x358F423,0x3486422, 0xC0,0x10, 0xB,-24 }, // 1308: f13GM86; f50GM86; Lead 7 fifths
    { 0x0EDF331,0x07DF131, 0xCB,0x00, 0x8,+0 }, // 1309: f13GM87; f50GM87; Lead 8 brass
    { 0x395FF09,0x02552E1, 0xC0,0x00, 0x0,+0 }, // 1310: f13GM88; f50GM88; Pad 1 new age
    { 0x0052031,0x0063031, 0x58,0x40, 0x0,+0 }, // 1311: f13GM89; f50GM89; Pad 2 warm
    { 0x0735421,0x008F021, 0x0E,0x07, 0xA,+0 }, // 1312: f13GM90; f50GM90; Pad 3 polysynth
    { 0x00330B1,0x00440B2, 0x5D,0x00, 0x0,+0 }, // 1313: f13GM91; f50GM91; Pad 4 choir
    { 0x2023034,0x003F021, 0x27,0x09, 0xE,+0 }, // 1314: f13GM92; f50GM92; Pad 5 bowedpad
    { 0x3042001,0x2042030, 0x63,0x00, 0x0,+0 }, // 1315: f13GM93; f50GM93; Pad 6 metallic
    { 0x0585201,0x03641A1, 0x99,0x00, 0x6,+0 }, // 1316: f13GM94; f50GM94; Pad 7 halo
    { 0x0261131,0x0071031, 0x1B,0x00, 0xC,+0 }, // 1317: f13GM95; f50GM95; Pad 8 sweep
    { 0x0B4F291,0x075F101, 0xD0,0x00, 0x0,+0 }, // 1318: f13GM96; f50GM96; FX 1 rain
    { 0x0572132,0x0194263, 0x06,0x00, 0x9,-12 }, // 1319: f13GM97; f50GM97; FX 2 soundtrack
    { 0x3859F85,0x043F311, 0x15,0x00, 0xE,+0 }, // 1320: f13GM98; f50GM98; FX 3 crystal
    { 0x115F403,0x0C8F221, 0xD7,0x00, 0xA,+0 }, // 1321: f13GM99; f50GM99; FX 4 atmosphere
    { 0x295F300,0x2B9F2A0, 0x11,0x00, 0x0,+0 }, // 1322: f13GM100; f50GM100; FX 5 brightness
    { 0x0050021,0x2041020, 0xCF,0x00, 0x0,+0 }, // 1323: f13GM101; f50GM101; FX 6 goblins
    { 0x2A3F400,0x2B9F2A0, 0x1B,0x00, 0x0,+0 }, // 1324: f13GM102; f50GM102; FX 7 echoes
    { 0x0644312,0x2028030, 0x22,0x00, 0xE,+0 }, // 1325: f13GM103; f50GM103; FX 8 sci-fi
    { 0x098F201,0x1D5F307, 0x40,0x09, 0x0,+0 }, // 1326: f13GM104; f50GM104; Sitar
    { 0x083FF00,0x166F502, 0x00,0x00, 0xE,-12 }, // 1327: f13GM105; f50GM105; Banjo
    { 0x275FF12,0x2E8F310, 0x80,0x00, 0xE,+0 }, // 1328: f13GM106; f50GM106; Shamisen
    { 0x163F402,0x164F502, 0x0F,0x00, 0x0,-12 }, // 1329: f13GM107; f50GM107; Koto
    { 0x064FB05,0x2579600, 0xC9,0x00, 0x0,+0 }, // 1330: f13GM108; f50GM108; Kalimba
    { 0x1B2FF13,0x30F5030, 0x0C,0x0A, 0xE,+0 }, // 1331: f13GM109; f50GM109; Bagpipe
    { 0x21DF230,0x10C4021, 0x0E,0x00, 0xA,+0 }, // 1332: f13GM110; f50GM110; Fiddle
    { 0x3023030,0x2064030, 0xC0,0x00, 0x0,+0 }, // 1333: f13GM111; f50GM111; Shanai
    { 0x375FF25,0x033FE03, 0xC0,0x00, 0x0,-7 }, // 1334: f13GM112; f50GM112; Tinkle Bell
    { 0x37DFE25,0x0079003, 0xC0,0x00, 0x0,-7 }, // 1335: f13GM113; f50GM113; Agogo Bells
    { 0x0034007,0x0056001, 0xDC,0x00, 0x0,+0 }, // 1336: f13GM114; f50GM114; Steel Drums
    { 0x0A3FD07,0x078F902, 0xC0,0x00, 0xE,+0 }, // 1337: f13GM115; f13GP76; f13GP77; f50GM115; f50GP76; f50GP77; High Wood Block; Low Wood Block; Woodblock
    { 0x2B3F811,0x003F010, 0xC1,0x03, 0x4,-7 }, // 1338: f13GM116; f50GM116; Taiko Drum
    { 0x00CF000,0x006F000, 0x00,0x00, 0x4,+2 }, // 1339: f13GM117; f50GM117; Melodic Tom
    { 0x32C8F01,0x006F000, 0x00,0x00, 0xE,+0 }, // 1340: f13GM118; f50GM118; Synth Drum
    { 0x2A2FF80,0x30E108E, 0x00,0x00, 0xE,+0 }, // 1341: f13GM119; f50GM119; Reverse Cymbal
    { 0x05C5F0E,0x16C870E, 0x00,0x02, 0x0,+0 }, // 1342: f13GM120; f50GM120; Guitar FretNoise
    { 0x092FF11,0x306301E, 0xC0,0x00, 0xE,+0 }, // 1343: f13GM121; f50GM121; Breath Noise
    { 0x003402E,0x003109E, 0x00,0x00, 0xE,+0 }, // 1344: f13GM122; f50GM122; Seashore
    { 0x2A3379B,0x237461A, 0x95,0x40, 0x0,+0 }, // 1345: f13GM123; f50GM123; Bird Tweet
    { 0x344FFAB,0x02AF1EA, 0xC0,0x01, 0xC,-12 }, // 1346: f13GM124; f50GM124; Telephone
    { 0x10EF0BE,0x00E3030, 0x00,0x0A, 0xE,+0 }, // 1347: f13GM125; f50GM125; Helicopter
    { 0x003F02E,0x00310FE, 0x00,0x00, 0xE,+0 }, // 1348: f13GM126; f50GM126; Applause/Noise
    { 0x023FCC0,0x006F08E, 0x00,0x00, 0xE,+0 }, // 1349: f13GM127; f50GM127; Gunshot
    { 0x0A3FB00,0x007F000, 0xC0,0x00, 0xA,+0 }, // 1350: f13GP35; f13GP36; f50GP35; f50GP36; Ac Bass Drum; Bass Drum 1
    { 0x0C2FD05,0x3D9F910, 0xC0,0x00, 0x0,+0 }, // 1351: f13GP37; f50GP37; Side Stick
    { 0x03A8F2E,0x067A800, 0x00,0x00, 0xE,+0 }, // 1352: f13GP38; f50GP38; Acoustic Snare
    { 0x22C8305,0x0589903, 0x00,0x00, 0xE,+0 }, // 1353: f13GP39; f50GP39; Hand Clap
    { 0x25C8400,0x08AF800, 0x00,0x00, 0xE,+0 }, // 1354: f13GP40; f50GP40; Electric Snare
    { 0x00CFF00,0x006FF00, 0x00,0x00, 0x4,+0 }, // 1355: f13GP41; f13GP43; f13GP45; f13GP47; f13GP48; f13GP50; f50GP41; f50GP43; f50GP45; f50GP47; f50GP48; f50GP50; High Floor Tom; High Tom; High-Mid Tom; Low Floor Tom; Low Tom; Low-Mid Tom
    { 0x004F081,0x308F009, 0xC0,0x00, 0xE,+0 }, // 1356: f13GP42; f50GP42; Closed High Hat
    { 0x006F001,0x339880D, 0x40,0x00, 0xC,+0 }, // 1357: f13GP44; f50GP44; Pedal High Hat
    { 0x12FF201,0x356F58E, 0xC0,0x00, 0xE,+0 }, // 1358: f13GP46; f50GP46; Open High Hat
    { 0x12FF281,0x356F58E, 0xC0,0x00, 0xE,+0 }, // 1359: f13GP49; f13GP57; f50GP49; f50GP57; Crash Cymbal 1; Crash Cymbal 2
    { 0x155AF00,0x364FF8B, 0x00,0x00, 0xE,+0 }, // 1360: f13GP51; f13GP53; f13GP59; f50GP51; f50GP53; f50GP59; Ride Bell; Ride Cymbal 1; Ride Cymbal 2
    { 0x1496401,0x356F58A, 0xC0,0x00, 0xE,+0 }, // 1361: f13GP52; f50GP52; Chinese Cymbal
    { 0x2678900,0x357878E, 0x00,0x00, 0xE,+0 }, // 1362: f13GP54; f50GP54; Tambourine
    { 0x02FF281,0x356F58E, 0xC0,0x00, 0x0,+0 }, // 1363: f13GP55; f50GP55; Splash Cymbal
    { 0x152FE09,0x008F002, 0xC0,0x00, 0xE,+0 }, // 1364: f13GP56; f50GP56; Cow Bell
    { 0x05FF210,0x27FC40E, 0x00,0x00, 0x6,+0 }, // 1365: f13GP58; f50GP58; Vibraslap
    { 0x00CF003,0x03AF802, 0xC0,0x00, 0x0,+0 }, // 1366: f13GP60; f50GP60; High Bongo
    { 0x00BF003,0x037F702, 0xC0,0x00, 0x0,+0 }, // 1367: f13GP61; f50GP61; Low Bongo
    { 0x00CF003,0x01AFD02, 0xC0,0x00, 0xE,+0 }, // 1368: f13GP62; f50GP62; Mute High Conga
    { 0x00BF002,0x037F702, 0xC0,0x00, 0x0,+0 }, // 1369: f13GP63; f13GP64; f50GP63; f50GP64; Low Conga; Open High Conga
    { 0x1779A01,0x084F700, 0x00,0x00, 0x8,+0 }, // 1370: f13GP65; f13GP66; f50GP65; f50GP66; High Timbale; Low Timbale
    { 0x325FF25,0x0078003, 0xC0,0x00, 0x0,+0 }, // 1371: f13GP67; f13GP68; f50GP67; f50GP68; High Agogo; Low Agogo
    { 0x0089011,0x357898E, 0xC0,0x00, 0xE,+0 }, // 1372: f13GP69; f50GP69; Cabasa
    { 0x11BF100,0x3468B9E, 0x00,0x00, 0xE,+0 }, // 1373: f13GP70; f50GP70; Maracas
    { 0x205504C,0x05C859D, 0x80,0x0A, 0xA,+0 }, // 1374: f13GP71; f50GP71; Short Whistle
    { 0x205508C,0x05C854D, 0x40,0x0A, 0x0,+0 }, // 1375: f13GP72; f50GP72; Long Whistle
    { 0x206F08B,0x346F610, 0x00,0x00, 0xE,+0 }, // 1376: f13GP73; f50GP73; Short Guiro
    { 0x392F700,0x2AF479E, 0x00,0x00, 0xE,+0 }, // 1377: f13GP74; f50GP74; Long Guiro
    { 0x30FF01D,0x0F0F715, 0x00,0x00, 0x1,+0 }, // 1378: f13GP75; f50GP75; Claves
    { 0x0EB3402,0x0075004, 0x87,0x00, 0x0,+0 }, // 1379: f13GP78; f50GP78; Mute Cuica
    { 0x0EF3301,0x0075002, 0xCB,0x00, 0x0,+0 }, // 1380: f13GP79; f50GP79; Open Cuica
    { 0x2B2FF04,0x2188719, 0x80,0x04, 0x0,+0 }, // 1381: f13GP80; f50GP80; Mute Triangle
    { 0x27FFF06,0x204F009, 0x80,0x0A, 0x0,+0 }, // 1382: f13GP81; f50GP81; Open Triangle
    { 0x053F300,0x247698E, 0x43,0x00, 0xE,+0 }, // 1383: f13GP82; f50GP82; Shaker
    { 0x224F10E,0x335FF8E, 0x40,0x02, 0x0,+0 }, // 1384: f13GP83; f13GP84; f50GP83; f50GP84; Bell Tree; Jingle Bell
    { 0x3948F03,0x06FFA15, 0x00,0x00, 0x0,+0 }, // 1385: f13GP85; f50GP85; Castanets
    { 0x274F911,0x108F010, 0x41,0x00, 0x2,+0 }, // 1386: f13GP86; f50GP86; Mute Surdu
    { 0x288F911,0x004F010, 0xC1,0x03, 0x4,+0 }, // 1387: f13GP87; f50GP87; Open Surdu
    { 0x15DFD25,0x0079003, 0xC0,0x00, 0x0,+0 }, // 1388: f13GP88; f50GP88
    { 0x015FF0E,0x0BFF800, 0x00,0x00, 0xE,+0 }, // 1389: f13GP89; f50GP89
    { 0x008A000,0x1679810, 0x00,0x00, 0xE,+0 }, // 1390: f13GP90; f50GP90
    { 0x104F081,0x308F009, 0xC0,0x00, 0xE,+0 }, // 1391: f13GP91; f50GP91
    { 0x053F101,0x074F131, 0x4B,0x00, 0x4,+0 }, // 1392: f15GM0; f26GM0; AcouGrandPiano
    { 0x053F201,0x064F311, 0x49,0x00, 0x6,+0 }, // 1393: f15GM1; f26GM1; BrightAcouGrand
    { 0x053F201,0x064F331, 0x50,0x00, 0x4,+0 }, // 1394: f15GM2; f26GM2; ElecGrandPiano
    { 0x078C423,0x048C231, 0x99,0x00, 0x8,+0 }, // 1395: f15GM3; f26GM3; Honky-tonkPiano
    { 0x098C423,0x058C231, 0x97,0x00, 0x6,+0 }, // 1396: f15GM4; f26GM4; Rhodes Piano
    { 0x088C423,0x048C231, 0x5E,0x00, 0x0,+0 }, // 1397: f15GM5; f26GM5; Chorused Piano
    { 0x05AC421,0x03AC231, 0x4E,0x00, 0x6,+0 }, // 1398: f15GM6; f26GM6; Harpsichord
    { 0x056B301,0x056B301, 0x8D,0x00, 0x8,+0 }, // 1399: f15GM7; f26GM7; Clavinet
    { 0x019D0A3,0x017F021, 0x5C,0x80, 0xC,+0 }, // 1400: f15GM8; f26GM8; Celesta
    { 0x018D0A3,0x018F021, 0x64,0x80, 0x0,+0 }, // 1401: f15GM9; f26GM9; Glockenspiel
    { 0x018F6B3,0x008F131, 0x61,0x00, 0x2,+0 }, // 1402: f15GM10; f26GM10; Music box
    { 0x09EAAB3,0x03E80A1, 0x08,0x00, 0x6,+0 }, // 1403: f15GM11; f26GM11; Vibraphone
    { 0x1239723,0x0144571, 0x93,0x00, 0x4,+0 }, // 1404: f15GM12; f26GM12; Marimba
    { 0x12497A1,0x0145571, 0x0D,0x80, 0x2,+0 }, // 1405: f15GM13; f26GM13; Xylophone
    { 0x1249761,0x0144571, 0x8F,0x00, 0xA,+0 }, // 1406: f15GM14; f26GM14; Tubular Bells
    { 0x0297721,0x00B9721, 0x89,0x80, 0x6,+0 }, // 1407: f15GM24; Acoustic Guitar1
    { 0x24D7520,0x01D8921, 0x8B,0x80, 0xA,+0 }, // 1408: f15GM25; f26GM25; Acoustic Guitar2
    { 0x01C6421,0x03CD621, 0xC4,0x00, 0xA,+0 }, // 1409: f15GM26; f26GM26; Electric Guitar1
    { 0x03C6421,0x01CA621, 0x4A,0x00, 0x8,+0 }, // 1410: f15GM27; f26GM27; Electric Guitar2
    { 0x008F321,0x228F322, 0x92,0x80, 0xA,+0 }, // 1411: f15GM29; f26GM29; Overdrive Guitar
    { 0x028F331,0x038B1B1, 0x92,0x00, 0xA,+0 }, // 1412: f15GM31; f26GM31; Guitar Harmonics
    { 0x002DB77,0x0125831, 0xE0,0x00, 0x8,+0 }, // 1413: f15GM32; f26GM32; Acoustic Bass
    { 0x2556823,0x1055461, 0xD2,0x00, 0xA,+0 }, // 1414: f15GM35; f26GM35; Fretless Bass
    { 0x1D6FB34,0x0269471, 0x83,0x00, 0xC,+0 }, // 1415: f15GM37; f26GM37; Slap Bass 2
    { 0x0096821,0x01B5731, 0x11,0x80, 0xA,+0 }, // 1416: f15GM39; f26GM39; Synth Bass 2
    { 0x078F71A,0x0024691, 0xC6,0x00, 0x2,+0 }, // 1417: f15GM41; f26GM41; Viola
    { 0x0287C31,0x01AAB23, 0x91,0x00, 0xA,+0 }, // 1418: f15GM42; f26GM42; Cello
    { 0x118D671,0x018F571, 0x1E,0x00, 0xC,+0 }, // 1419: f15GM44; f26GM44; Tremulo Strings
    { 0x0287271,0x0186361, 0x95,0x00, 0xC,+0 }, // 1420: f15GM45; f26GM45; Pizzicato String
    { 0x054F589,0x023F582, 0x5E,0x07, 0x2,+0 }, // 1421: f15GM46; f26GM46; Orchestral Harp
    { 0x125F121,0x0087262, 0x56,0x00, 0xE,+0 }, // 1422: f15GM48; f26GM48; String Ensemble1
    { 0x1388231,0x0086821, 0x4B,0x00, 0x0,+0 }, // 1423: f15GM50; f26GM50; Synth Strings 1
    { 0x11561B1,0x00562A1, 0x16,0x00, 0x8,+0 }, // 1424: f15GM52; f26GM52; Choir Aahs
    { 0x01351A1,0x0175221, 0x1E,0x80, 0xE,+0 }, // 1425: f15GM53; f26GM53; Voice Oohs
    { 0x1145131,0x00552A1, 0x92,0x00, 0xA,+0 }, // 1426: f15GM54; f26GM54; Synth Voice
    { 0x12CF131,0x01C61B1, 0x8F,0x00, 0x8,+0 }, // 1427: f15GM55; f26GM55; Orchestra Hit
    { 0x1228131,0x0167223, 0x4D,0x80, 0x2,+0 }, // 1428: f15GM56; f26GM56; Trumpet
    { 0x171D201,0x238F301, 0x55,0x00, 0x2,+0 }, // 1429: f15GM59; f26GM59; Muted Trumpet
    { 0x114F413,0x013F201, 0x49,0x80, 0x6,+0 }, // 1430: f15GM60; f26GM60; French Horn
    { 0x154F203,0x044F301, 0x4C,0x40, 0x4,+0 }, // 1431: f15GM61; f26GM61; Brass Section
    { 0x119F523,0x019F421, 0x51,0x00, 0xC,+0 }, // 1432: f15GM62; f26GM62; Synth Brass 1
    { 0x1547003,0x004B301, 0x51,0x80, 0xC,+0 }, // 1433: f15GM63; f26GM63; Synth Brass 2
    { 0x018F221,0x018F521, 0x0F,0x80, 0x6,+0 }, // 1434: f15GM66; f26GM66; Tenor Sax
    { 0x038F2A1,0x018F321, 0x93,0x00, 0xA,+0 }, // 1435: f15GM67; f26GM67; Baritone Sax
    { 0x13FF631,0x01FF321, 0x89,0x40, 0xA,+0 }, // 1436: f15GM68; f26GM68; Oboe
    { 0x13FF431,0x01FF221, 0x88,0x40, 0xA,+0 }, // 1437: f15GM69; f26GM69; English Horn
    { 0x05F8571,0x01A6661, 0x51,0x00, 0xC,+0 }, // 1438: f15GM77; f26GM77; Shakuhachi
    { 0x13F93B1,0x01F6221, 0x45,0x80, 0x8,+0 }, // 1439: f15GM78; f26GM78; Whistle
    { 0x13FA3B1,0x00F8221, 0x89,0x80, 0x8,+0 }, // 1440: f15GM79; f26GM79; Ocarina
    { 0x13F86B1,0x00F7221, 0x8F,0x80, 0xC,+0 }, // 1441: f15GM80; f26GM80; Lead 1 squareea
    { 0x137C6B1,0x0067221, 0x87,0x80, 0xC,+0 }, // 1442: f15GM81; f26GM81; Lead 2 sawtooth
    { 0x0069161,0x0076161, 0x12,0x00, 0xA,+0 }, // 1443: f15GM87; f26GM87; Lead 8 brass
    { 0x12DC331,0x00F7861, 0x8A,0x00, 0xA,+0 }, // 1444: f15GM88; Pad 1 new age
    { 0x13DC231,0x00F7761, 0x8A,0x80, 0xA,+0 }, // 1445: f15GM89; f26GM89; Pad 2 warm
    { 0x02DF431,0x00F7321, 0x8B,0x80, 0x6,+0 }, // 1446: f15GM90; f26GM90; Pad 3 polysynth
    { 0x02DA831,0x00F8321, 0x8B,0x80, 0x6,+0 }, // 1447: f15GM91; f26GM91; Pad 4 choir
    { 0x07A6161,0x00AC121, 0x99,0x80, 0x4,+0 }, // 1448: f15GM92; Pad 5 bowedpad
    { 0x07A6161,0x00AC121, 0x9A,0x80, 0x4,+0 }, // 1449: f15GM93; Pad 6 metallic
    { 0x01C8D21,0x00FA521, 0x90,0x00, 0xA,+0 }, // 1450: f15GM94; f26GM94; Pad 7 halo
    { 0x01F75A1,0x00F7422, 0x10,0x00, 0x8,+0 }, // 1451: f15GM95; f26GM95; Pad 8 sweep
    { 0x11F75A0,0x01F7521, 0x15,0x00, 0xC,+0 }, // 1452: f15GM96; f26GM96; FX 1 rain
    { 0x013F5C5,0x005FDE1, 0x59,0x80, 0xA,+0 }, // 1453: f15GM98; f26GM98; FX 3 crystal
    { 0x0248305,0x014A301, 0x66,0x00, 0x2,+0 }, // 1454: f15GM99; f26GM99; FX 4 atmosphere
    { 0x031A585,0x011F511, 0xD3,0x80, 0x2,+0 }, // 1455: f15GM100; f26GM100; FX 5 brightness
    { 0x033F284,0x022F211, 0xC7,0x80, 0xA,+0 }, // 1456: f15GM101; f26GM101; FX 6 goblins
    { 0x122F210,0x012FC11, 0xC9,0x00, 0x6,+0 }, // 1457: f15GM102; f26GM102; FX 7 echoes
    { 0x055FC14,0x005F311, 0x8D,0x00, 0xE,+0 }, // 1458: f15GM103; f26GM103; FX 8 sci-fi
    { 0x206FB03,0x006D901, 0xD2,0x00, 0x4,+0 }, // 1459: f15GM104; f26GM104; Sitar
    { 0x024D443,0x004E741, 0x51,0x40, 0x8,+0 }, // 1460: f15GM105; f26GM105; Banjo
    { 0x0275722,0x0275661, 0x59,0x40, 0xB,+0 }, // 1461: f15GM108; f26GM108; Kalimba
    { 0x0175622,0x0176361, 0xA7,0x40, 0x5,+0 }, // 1462: f15GM109; f26GM109; Bagpipe
    { 0x205A8F1,0x00563B1, 0x9B,0x00, 0xA,+0 }, // 1463: f15GM110; f26GM110; Fiddle
    { 0x05F8571,0x00A6B61, 0x4B,0x00, 0xC,+0 }, // 1464: f15GM111; f26GM111; Shanai
    { 0x105F510,0x0C3F211, 0x47,0x00, 0x2,+0 }, // 1465: f15GM112; f26GM112; Tinkle Bell
    { 0x247F811,0x054F311, 0x47,0x00, 0x4,+0 }, // 1466: f15GM113; f26GM113; Agogo Bells
    { 0x21AF400,0x008F800, 0x00,0x00, 0xC,+0 }, // 1467: f15GM114; f26GM114; Steel Drums
    { 0x01AF400,0x038F800, 0x00,0x00, 0xA,+0 }, // 1468: f15GM115; f26GM115; Woodblock
    { 0x079F400,0x017F600, 0x03,0x00, 0xA,+0 }, // 1469: f15GM116; f26GM116; Taiko Drum
    { 0x007A810,0x115DA00, 0x06,0x00, 0x6,+0 }, // 1470: f15GM117; f26GM117; Melodic Tom
    { 0x009A810,0x107DF10, 0x07,0x00, 0xE,+0 }, // 1471: f15GM118; f15GP39; f26GM118; f26GP39; Hand Clap; Synth Drum
    { 0x334F407,0x2D4F415, 0x00,0x00, 0xE,+0 }, // 1472: f15GM119; f26GM119; Reverse Cymbal
    { 0x0F4000A,0x0F6F717, 0x3F,0x00, 0x1,+0 }, // 1473: f15GM120; f26GM120; Guitar FretNoise
    { 0x0F2E00E,0x033FF1E, 0x5E,0x40, 0x8,+0 }, // 1474: f15GM121; f26GM121; Breath Noise
    { 0x0645451,0x045A581, 0x00,0x00, 0xA,+0 }, // 1475: f15GM122; f26GM122; Seashore
    { 0x261B235,0x0B2F112, 0x5C,0x08, 0xA,+0 }, // 1476: f15GM123; f26GM123; Bird Tweet
    { 0x38CF800,0x06BF600, 0x80,0x00, 0xF,+0 }, // 1477: f15GM125; f26GM125; Helicopter
    { 0x060F207,0x072F212, 0x54,0x80, 0x4,+0 }, // 1478: f15GM126; f26GM126; Applause/Noise
    { 0x0557542,0x0257541, 0x96,0x87, 0x8,+0 }, // 1479: f15GM127; f26GM127; Gunshot
    { 0x0F70700,0x0F70710, 0xFF,0xFF, 0x0,+0 }, // 1480: f15GP0; f15GP1; f15GP10; f15GP101; f15GP102; f15GP103; f15GP104; f15GP105; f15GP106; f15GP107; f15GP108; f15GP109; f15GP11; f15GP110; f15GP111; f15GP112; f15GP113; f15GP114; f15GP115; f15GP116; f15GP117; f15GP118; f15GP119; f15GP12; f15GP120; f15GP121; f15GP122; f15GP123; f15GP124; f15GP125; f15GP126; f15GP127; f15GP13; f15GP14; f15GP15; f15GP16; f15GP17; f15GP18; f15GP19; f15GP2; f15GP20; f15GP21; f15GP22; f15GP23; f15GP24; f15GP25; f15GP26; f15GP27; f15GP28; f15GP29; f15GP3; f15GP30; f15GP31; f15GP32; f15GP33; f15GP34; f15GP4; f15GP5; f15GP52; f15GP53; f15GP55; f15GP57; f15GP58; f15GP59; f15GP6; f15GP7; f15GP74; f15GP76; f15GP77; f15GP78; f15GP79; f15GP8; f15GP80; f15GP81; f15GP82; f15GP83; f15GP84; f15GP85; f15GP86; f15GP87; f15GP88; f15GP89; f15GP9; f15GP90; f15GP91; f15GP92; f15GP93; f15GP94; f15GP95; f15GP96; f15GP97; f15GP98; f15GP99; f26GP0; f26GP1; f26GP10; f26GP101; f26GP102; f26GP103; f26GP104; f26GP105; f26GP106; f26GP107; f26GP108; f26GP109; f26GP11; f26GP110; f26GP111; f26GP112; f26GP113; f26GP114; f26GP115; f26GP116; f26GP117; f26GP118; f26GP119; f26GP12; f26GP120; f26GP121; f26GP122; f26GP123; f26GP124; f26GP125; f26GP126; f26GP127; f26GP13; f26GP14; f26GP15; f26GP16; f26GP17; f26GP18; f26GP19; f26GP2; f26GP20; f26GP21; f26GP22; f26GP23; f26GP24; f26GP25; f26GP26; f26GP27; f26GP28; f26GP29; f26GP3; f26GP30; f26GP31; f26GP32; f26GP33; f26GP34; f26GP4; f26GP5; f26GP52; f26GP53; f26GP55; f26GP57; f26GP58; f26GP59; f26GP6; f26GP7; f26GP74; f26GP76; f26GP77; f26GP78; f26GP79; f26GP8; f26GP80; f26GP81; f26GP82; f26GP83; f26GP84; f26GP85; f26GP86; f26GP87; f26GP88; f26GP89; f26GP9; f26GP90; f26GP91; f26GP92; f26GP93; f26GP94; f26GP95; f26GP96; f26GP97; f26GP98; f26GP99; Bell Tree; Castanets; Chinese Cymbal; Crash Cymbal 2; High Wood Block; Jingle Bell; Long Guiro; Low Wood Block; Mute Cuica; Mute Surdu; Mute Triangle; Open Cuica; Open Surdu; Open Triangle; Ride Bell; Ride Cymbal 2; Shaker; Splash Cymbal; Vibraslap
    { 0x268F911,0x005F211, 0x46,0x00, 0x8,+0 }, // 1481: f15GP35; f15GP36; f26GP35; f26GP36; Ac Bass Drum; Bass Drum 1
    { 0x14BFA01,0x03BFA08, 0x08,0x00, 0xD,+0 }, // 1482: f15GP37; f26GP37; Side Stick
    { 0x007FF21,0x107F900, 0x80,0x00, 0xE,+0 }, // 1483: f15GP38; f26GP38; Acoustic Snare
    { 0x20DFF20,0x027FF02, 0x00,0x00, 0xE,+0 }, // 1484: f15GP40; f26GP40; Electric Snare
    { 0x0C8F60C,0x257FF12, 0xC2,0x00, 0xC,+0 }, // 1485: f15GP42; f26GP42; Closed High Hat
    { 0x2B7F811,0x003F310, 0x45,0x00, 0x8,+0 }, // 1486: f15GP56; f26GP56; Cow Bell
    { 0x08DFA01,0x0BAFA03, 0x4F,0x00, 0x7,+0 }, // 1487: f15GP62; f26GP62; Mute High Conga
    { 0x38FF801,0x06FF600, 0x47,0x00, 0xF,+0 }, // 1488: f15GP65; f26GP65; High Timbale
    { 0x38CF800,0x06EF600, 0x80,0x00, 0xF,+0 }, // 1489: f15GP66; f26GP66; Low Timbale
    { 0x38CF803,0x0B5F80C, 0x80,0x00, 0xF,+0 }, // 1490: f15GP67; f26GP67; High Agogo
    { 0x38CF803,0x0B5F80C, 0x83,0x00, 0xF,+0 }, // 1491: f15GP68; f26GP68; Low Agogo
    { 0x0DFF611,0x0DEF710, 0x4F,0x40, 0xC,+0 }, // 1492: f15GP73; f26GP73; Short Guiro
    { 0x21351A0,0x2275360, 0x98,0x01, 0xE,+0 }, // 1493: f16GM41; f37GM41; f54GM41; Viola
    { 0x075F502,0x0F3F201, 0x20,0x83, 0xC,+0 }, // 1494: f16GM46; f37GM46; f54GM46; Orchestral Harp
    { 0x025C5A2,0x005EF24, 0x20,0x9F, 0xE,-12 }, // 1495: f16GM48; f54GM48; String Ensemble1
    { 0x004EF26,0x0068F24, 0x9C,0x02, 0xE,+0 }, // 1496: f16GM48; f54GM48; String Ensemble1
    { 0x0064131,0x03892A1, 0x1C,0x80, 0xE,+0 }, // 1497: f16GM56; f54GM56; Trumpet
    { 0x0064131,0x02882A1, 0x1B,0x80, 0xF,+0 }, // 1498: f16GM56; f54GM56; Trumpet
    { 0x0156220,0x0267321, 0x98,0x00, 0xE,+12 }, // 1499: f16GM58; f54GM58; Tuba
    { 0x02651B1,0x0265171, 0xD1,0x00, 0xF,+0 }, // 1500: f16GM58; f54GM58; Tuba
    { 0x0766321,0x0167CA1, 0x93,0x00, 0xC,+0 }, // 1501: f16GM60; f54GM60; French Horn
    { 0x1168321,0x0269CA1, 0x4D,0x00, 0xD,+0 }, // 1502: f16GM60; f54GM60; French Horn
    { 0x04CA900,0x04FF600, 0x07,0x00, 0xA,+0 }, // 1503: f16GP35; Ac Bass Drum
    { 0x075F80F,0x2B78A03, 0x80,0x00, 0xE,+0 }, // 1504: f16GP70; Maracas
    { 0x059A490,0x4C86590, 0x0B,0x00, 0xE,+0 }, // 1505: f16GP73; Short Guiro
    { 0x055A210,0x4766600, 0x0A,0x00, 0xE,+0 }, // 1506: f16GP74; Long Guiro
    { 0x059FA00,0x09AF500, 0x05,0x00, 0x6,+0 }, // 1507: f16GP87; Open Surdu
    { 0x0001F0E,0x3F01FC0, 0x00,0x00, 0xE,+0 }, // 1508: f17GM119; f35GM119; Reverse Cymbal
    { 0x2129A14,0x004FA01, 0x97,0x80, 0xE,+0 }, // 1509: b41M2; f19GM2; f21GM2; f41GM2; ElecGrandPiano; elecvibe
    { 0x0038165,0x007F171, 0xD2,0x00, 0x2,+0 }, // 1510: b41M14; b41M3; f19GM14; f19GM3; f21GM14; f41GM14; Honky-tonkPiano; Tubular Bells; pipes.in
    { 0x0AE7121,0x01ED320, 0x1C,0x00, 0xE,+0 }, // 1511: b41M4; f19GM4; Rhodes Piano; circus.i
    { 0x035F813,0x004FF11, 0x12,0x00, 0x8,+0 }, // 1512: b41M8; f19GM8; f21GM8; f41GM8; Celesta; SB8.ins
    { 0x00FFF24,0x00FFF21, 0x00,0x40, 0x1,+0 }, // 1513: b41M10; f19GM10; f41GM10; 60sorgan; Music box
    { 0x0F0FB3E,0x09BA0B1, 0x29,0x00, 0x0,+0 }, // 1514: f19GM11; Vibraphone
    { 0x275F602,0x066F521, 0x9B,0x00, 0x4,+0 }, // 1515: b41M12; f19GM12; f21GM12; f41GM12; Marimba; SB12.ins
    { 0x315EF11,0x0B5F441, 0x53,0x00, 0x8,+0 }, // 1516: f19GM13; Xylophone
    { 0x10BF224,0x00B5231, 0x50,0x00, 0xE,+0 }, // 1517: b41M15; f19GM15; f21GM15; f41GM15; Dulcimer; pirate.i
    { 0x0AFF832,0x07FF310, 0x45,0x00, 0xE,+0 }, // 1518: b41M19; f19GM19; f21GM19; f41GM19; Church Organ; logdrum1
    { 0x0F7F000,0x0068761, 0x30,0x00, 0xF,+0 }, // 1519: f19GM21; Accordion
    { 0x275F602,0x066F521, 0x1B,0x00, 0x4,+0 }, // 1520: b41M24; f19GM24; f21GM24; f41GM24; Acoustic Guitar1; SB24.ins
    { 0x1FFF000,0x1FFF001, 0x0A,0x00, 0xE,+0 }, // 1521: f19GM25; f41GM25; Acoustic Guitar2
    { 0x141B403,0x03FF311, 0x5E,0x00, 0xA,+0 }, // 1522: b41M26; f19GM26; f21GM26; f41GM26; Electric Guitar1; SB26.ins
    { 0x1EFF201,0x078F501, 0x1D,0x00, 0x6,+0 }, // 1523: b41M32; f19GM32; f21GM32; f41GM32; Acoustic Bass; SB32.ins
    { 0x2831621,0x0C31320, 0xDA,0x00, 0x8,+0 }, // 1524: f19GM33; f19GM36; Electric Bass 1; Slap Bass 1
    { 0x061F217,0x076F212, 0x4F,0x00, 0x8,+0 }, // 1525: b41M38; f19GM38; f41GM38; Synth Bass 1; trainbel
    { 0x2298432,0x0448421, 0x1A,0x00, 0x6,+0 }, // 1526: b41M41; f19GM41; f21GM41; f41GM41; SB41.ins; Viola
    { 0x0157261,0x0278461, 0x1C,0x00, 0xE,+0 }, // 1527: f19GM43; Contrabass
    { 0x0117171,0x11562A1, 0x8B,0x00, 0x6,+0 }, // 1528: f19GM48; f19GM50; String Ensemble1; Synth Strings 1
    { 0x2D3B121,0x0149121, 0x4F,0x80, 0x6,+0 }, // 1529: b41M94; f19GM49; f19GM94; f41GM94; Pad 7 halo; SB94.ins; String Ensemble2
    { 0x095AB0E,0x0C6F702, 0xC0,0x00, 0xE,+0 }, // 1530: b41M105; b41M51; f19GM105; f19GM51; f21GM105; f41GM105; f41GM51; Banjo; SynthStrings 2; koto1.in
    { 0x0176E30,0x12F8B32, 0x4B,0x05, 0x4,+0 }, // 1531: b41M71; f19GM71; f41GM71; Clarinet; SB71.ins
    { 0x08F7461,0x02A6561, 0x27,0x00, 0x2,+0 }, // 1532: b41M72; b41M73; b41M74; b41M75; f19GM72; f19GM73; f19GM74; f19GM75; f21GM72; f21GM75; f41GM72; f41GM73; f41GM74; f41GM75; Flute; Pan Flute; Piccolo; Recorder; flute1.i
    { 0x0EBFA10,0x0DAFA0E, 0x00,0x00, 0x0,+0 }, // 1533: b41M76; b41M78; f19GM76; f41GM76; Bottle Blow; cowboy2.
    { 0x0F7F0F5,0x00687B1, 0x2E,0x00, 0xB,+0 }, // 1534: f19GM77; Shakuhachi
    { 0x3DFFF20,0x20FFF21, 0x00,0x00, 0x0,+0 }, // 1535: b41M81; f19GM81; f27GM81; f41GM81; Lead 2 sawtooth; SB81.ins
    { 0x000FF24,0x00BF020, 0x97,0x00, 0x4,+0 }, // 1536: b41M82; f19GM82; f41GM82; Lead 3 calliope; airplane
    { 0x203B122,0x005F172, 0x4F,0x00, 0x2,+0 }, // 1537: b41M87; f19GM87; Lead 8 brass; harmonca
    { 0x0177421,0x0176562, 0x83,0x00, 0x7,+0 }, // 1538: f19GM107; f19GM108; f19GM109; f19GM93; Bagpipe; Kalimba; Koto; Pad 6 metallic
    { 0x2129A16,0x0039A12, 0x97,0x00, 0x2,+0 }, // 1539: b41M96; f19GM96; FX 1 rain; SB96.ins
    { 0x01FF003,0x019F000, 0x1F,0x05, 0xA,+0 }, // 1540: b41M97; f19GM97; f21GM97; f41GM97; FX 2 soundtrack; organ3a.
    { 0x112AA43,0x1119B51, 0x1C,0x00, 0xE,+0 }, // 1541: f19GM100; FX 5 brightness
    { 0x0AC9011,0x1F4F071, 0x1A,0x00, 0xF,+0 }, // 1542: f19GM103; FX 8 sci-fi
    { 0x102FD16,0x0039F12, 0x96,0x80, 0xE,+0 }, // 1543: b41M113; f19GM113; f21GM113; f41GM113; Agogo Bells; SB113.in
    { 0x006FA15,0x025F501, 0xD3,0x00, 0xA,+0 }, // 1544: b41M115; f19GM115; SB115.in; Woodblock
    { 0x0F0F000,0x0048C2C, 0x2E,0x00, 0xE,+0 }, // 1545: b41M124; f19GM124; f41GM124; Telephone; chirp.in
    { 0x0111E00,0x0A11220, 0x00,0x00, 0x6,+0 }, // 1546: b41M125; f19GM125; f21GM125; f41GM125; Helicopter; SB125.in
    { 0x0F0F31E,0x0F6F610, 0x00,0x00, 0xE,+0 }, // 1547: b41P38; b41P60; f19GM126; f19GP38; f19GP60; f21GM126; f21GP60; f27GM127; f27GP38; f27GP39; f27GP40; f41GM126; f41GP38; Acoustic Snare; Applause/Noise; Electric Snare; Gunshot; Hand Clap; High Bongo; SBSN1.in
    { 0x011F111,0x061D001, 0x4A,0x40, 0x6,+0 }, // 1548: f19GM127; f21GM127; f41GM127; Gunshot
    { 0x04CA800,0x04FD600, 0x0B,0x00, 0x0,+0 }, // 1549: b41P36; f19GP35; f19GP36; f27GP36; f41GP36; Ac Bass Drum; Bass Drum 1; SBBD.ins
    { 0x282B264,0x1DA9803, 0x00,0x00, 0xE,+0 }, // 1550: f19GP42; Closed High Hat
    { 0x06F9A02,0x007A006, 0x00,0x00, 0x0,+0 }, // 1551: b41P48; b41P52; b41P53; f19GP48; f19GP52; f19GP53; f21GP48; f21GP52; f21GP53; f41GP48; f41GP52; f41GP53; Chinese Cymbal; High-Mid Tom; Ride Bell; tom2.ins
    { 0x0FFFB13,0x0FFE804, 0x40,0x00, 0x8,+0 }, // 1552: b41P49; b41P67; b41P68; f19GP49; f19GP63; f19GP67; f19GP68; f21GP49; f21GP67; f21GP68; f27GP32; f27GP33; f27GP34; f27GP37; f27GP67; f27GP68; f27GP75; f27GP85; f41GP49; f41GP60; f41GP67; f41GP68; Castanets; Claves; Crash Cymbal 1; High Agogo; High Bongo; Low Agogo; Open High Conga; Side Stick; claves.i
    { 0x3E5E40F,0x1E7F508, 0x00,0x0A, 0x6,+0 }, // 1553: b46P59; f20GP59; Ride Cymbal 2; gps059
    { 0x366F50F,0x1A8F608, 0x00,0x19, 0x7,+0 }, // 1554: b46P59; f20GP59; Ride Cymbal 2; gps059
    { 0x0F3F040,0x0038761, 0x30,0x00, 0xF,+0 }, // 1555: f21GM3; Honky-tonkPiano
    { 0x033E813,0x0F3F011, 0x12,0x00, 0x8,+0 }, // 1556: f21GM4; Rhodes Piano
    { 0x133F721,0x2F4F320, 0x48,0x00, 0x4,+0 }, // 1557: f21GM5; f41GM5; Chorused Piano
    { 0x1F4F201,0x0F5F009, 0x00,0x00, 0x6,+0 }, // 1558: f21GM9; Glockenspiel
    { 0x1114070,0x0034061, 0x84,0x00, 0x0,+0 }, // 1559: f21GM10; Music box
    { 0x0F0FB3E,0x09BA071, 0x29,0x00, 0x0,+0 }, // 1560: b41M11; f21GM11; Vibraphone; organ4.i
    { 0x315EF11,0x0B5F481, 0x53,0x00, 0x8,+0 }, // 1561: b41M13; f21GM13; f41GM13; SB13.ins; Xylophone
    { 0x000EA36,0x003D01A, 0x8B,0x00, 0x8,+0 }, // 1562: b41M16; f21GM16; f41GM16; Hammond Organ; harpsi7.
    { 0x1C3C223,0x103D000, 0x14,0x00, 0xC,+0 }, // 1563: b41M17; f21GM17; f27GM6; f41GM17; Harpsichord; Percussive Organ; harpsi6.
    { 0x0F7F000,0x00687A1, 0x30,0x00, 0xF,+0 }, // 1564: b41M21; f21GM21; f41GM21; Accordion; whistle.
    { 0x0009F71,0x1069F62, 0x45,0x00, 0x2,+0 }, // 1565: b41M22; f21GM22; f21GM54; f41GM22; Harmonica; Synth Voice; arabian2
    { 0x0009F71,0x1069062, 0x51,0x00, 0x0,+0 }, // 1566: b41M23; f21GM23; f27GM68; f41GM23; Oboe; Tango Accordion; arabian.
    { 0x0F7F001,0x00687A1, 0x00,0x00, 0x1,+0 }, // 1567: b41M25; f21GM25; Acoustic Guitar2; whistle2
    { 0x0D3B305,0x024F246, 0x40,0x80, 0x2,+0 }, // 1568: f21GM27; Electric Guitar2
    { 0x106F90E,0x0F4F001, 0x2F,0x00, 0xB,+0 }, // 1569: f21GM28; f41GM28; Electric Guitar3
    { 0x0126E71,0x0045061, 0x0D,0x00, 0x0,+0 }, // 1570: f21GM29; Overdrive Guitar
    { 0x2A31321,0x0F31220, 0x1A,0x00, 0x8,+0 }, // 1571: f21GM33; f41GM33; f41GM36; Electric Bass 1; Slap Bass 1
    { 0x025DC03,0x009F031, 0xA2,0x00, 0x8,+0 }, // 1572: f21GM34; Electric Bass 2
    { 0x025DC03,0x009F021, 0x17,0x00, 0x8,+0 }, // 1573: f21GM35; Fretless Bass
    { 0x025DF23,0x0F9F021, 0x20,0x00, 0xE,+0 }, // 1574: f21GM36; Slap Bass 1
    { 0x1025161,0x0024173, 0x52,0x00, 0xA,+0 }, // 1575: f21GM37; Slap Bass 2
    { 0x0195132,0x0396061, 0x5A,0x85, 0xC,+0 }, // 1576: f21GM38; Synth Bass 1
    { 0x025DC03,0x009F031, 0x9A,0x00, 0x8,+0 }, // 1577: f21GM39; Synth Bass 2
    { 0x025DC03,0x009F031, 0x98,0x00, 0x8,+0 }, // 1578: f21GM40; Violin
    { 0x1126EB1,0x0045021, 0x47,0x02, 0x0,+0 }, // 1579: f21GM42; f41GM42; Cello
    { 0x01572A1,0x02784A1, 0x1C,0x00, 0xE,+0 }, // 1580: b41M43; f21GM43; f41GM43; Contrabass; SB43.ins
    { 0x025DC03,0x009F031, 0x97,0x00, 0x8,+0 }, // 1581: f21GM44; Tremulo Strings
    { 0x025DC03,0x009F031, 0x96,0x00, 0x8,+0 }, // 1582: f21GM45; Pizzicato String
    { 0x025DC03,0x009F031, 0x94,0x00, 0x8,+0 }, // 1583: f21GM46; Orchestral Harp
    { 0x025DB02,0x006F030, 0x10,0x00, 0x8,+0 }, // 1584: f21GM47; Timpany
    { 0x1145152,0x0147242, 0x88,0x00, 0xA,+0 }, // 1585: f21GM48; String Ensemble1
    { 0x2F3F021,0x004F021, 0x4F,0x00, 0x6,+0 }, // 1586: b41M49; f21GM49; f41GM49; String Ensemble2; strnlong
    { 0x0115172,0x01572A2, 0x89,0x00, 0xA,+0 }, // 1587: f21GM50; f41GM48; f41GM50; String Ensemble1; Synth Strings 1
    { 0x0F8AF00,0x0F6F401, 0xC0,0x00, 0xE,+0 }, // 1588: f21GM51; SynthStrings 2
    { 0x0009FB1,0x1069FA2, 0x45,0x0D, 0x2,+0 }, // 1589: f21GM52; Choir Aahs
    { 0x0009FB1,0x1069FA2, 0x45,0x08, 0x2,+0 }, // 1590: f21GM53; Voice Oohs
    { 0x01152B0,0x0FE31B1, 0xC5,0x40, 0x0,+0 }, // 1591: b41M55; f21GM55; f32GM55; f41GM55; Orchestra Hit; cello.in
    { 0x1016F00,0x0F57001, 0x19,0x00, 0xE,+0 }, // 1592: f21GM61; f21GM88; f41GM88; Brass Section; Pad 1 new age
    { 0x103FF80,0x3FFF021, 0x01,0x00, 0x8,+0 }, // 1593: b41M62; f21GM62; f27GM30; f41GM62; Distorton Guitar; Synth Brass 1; elecgtr.
    { 0x229FFF2,0x0F480E1, 0x1A,0x00, 0x6,+0 }, // 1594: f21GM63; Synth Brass 2
    { 0x025DC03,0x009F032, 0x12,0x00, 0xA,+0 }, // 1595: f21GM64; Soprano Sax
    { 0x025DC03,0x009F032, 0x10,0x00, 0xA,+0 }, // 1596: f21GM65; Alto Sax
    { 0x025DC03,0x009F032, 0x0E,0x00, 0xA,+0 }, // 1597: f21GM66; Tenor Sax
    { 0x025DC03,0x009F032, 0x0C,0x00, 0xA,+0 }, // 1598: f21GM67; Baritone Sax
    { 0x025DC03,0x009F032, 0x0A,0x00, 0xA,+0 }, // 1599: f21GM68; Oboe
    { 0x025DC03,0x009F031, 0x92,0x00, 0x8,+0 }, // 1600: f21GM69; English Horn
    { 0x1062F01,0x0076521, 0x07,0x00, 0x0,+0 }, // 1601: f21GM70; Bassoon
    { 0x00470F5,0x0F38071, 0x1C,0x00, 0xB,+0 }, // 1602: f21GM71; Clarinet
    { 0x0F77061,0x0256061, 0x21,0x00, 0x2,+0 }, // 1603: f21GM73; Flute
    { 0x0C76012,0x00550F1, 0x28,0x00, 0x2,+0 }, // 1604: f21GM74; Recorder
    { 0x0049F21,0x0049F62, 0x00,0x00, 0x1,+0 }, // 1605: f21GM76; Bottle Blow
    { 0x0F7F0F5,0x0068771, 0x2E,0x00, 0xB,+0 }, // 1606: b41M77; f21GM77; f41GM77; Shakuhachi; afroflut
    { 0x2119A16,0x0029012, 0x14,0x00, 0x2,+0 }, // 1607: f21GM78; Whistle
    { 0x033F813,0x003FF11, 0x0E,0x00, 0x8,+0 }, // 1608: f21GM79; Ocarina
    { 0x0057F72,0x0F56071, 0x1D,0x00, 0x2,+0 }, // 1609: f21GM80; Lead 1 squareea
    { 0x203B162,0x005F172, 0x4A,0x00, 0x2,+0 }, // 1610: f21GM81; f21GM87; f41GM87; Lead 2 sawtooth; Lead 8 brass
    { 0x2027062,0x0029062, 0x4A,0x00, 0x2,+0 }, // 1611: f21GM82; Lead 3 calliope
    { 0x01B5132,0x03BA261, 0x9A,0x82, 0xC,+0 }, // 1612: b41M83; f21GM83; f32GM83; f37GM71; f41GM83; f47GM71; Clarinet; Lead 4 chiff; clarinet
    { 0x0176EB1,0x00E8BA2, 0xC5,0x05, 0x2,+0 }, // 1613: b41M84; b41M85; f21GM84; f21GM85; f32GM84; f32GM85; f41GM84; f41GM85; Lead 5 charang; Lead 6 voice; oboe.ins
    { 0x019D530,0x01B6171, 0xCD,0x40, 0xC,+0 }, // 1614: b41M86; f21GM86; f32GM86; f41GM86; Lead 7 fifths; bassoon.
    { 0x0FF0F20,0x0F1F021, 0xFF,0x00, 0x0,+0 }, // 1615: f21GM89; f21GM90; f21GM91; f21GM92; f21GM93; Pad 2 warm; Pad 3 polysynth; Pad 4 choir; Pad 5 bowedpad; Pad 6 metallic
    { 0x0F28021,0x0037021, 0x8F,0x00, 0x0,+0 }, // 1616: f21GM94; Pad 7 halo
    { 0x1F27021,0x0F68021, 0x14,0x00, 0xE,+0 }, // 1617: b41M95; f21GM95; f41GM95; Pad 8 sweep; brass1.i
    { 0x2129A16,0x0039012, 0x97,0x00, 0x2,+0 }, // 1618: f21GM96; FX 1 rain
    { 0x212AA93,0x021AC91, 0x97,0x80, 0xE,+0 }, // 1619: f21GM98; f32GM98; FX 3 crystal
    { 0x024DA05,0x013F901, 0x8B,0x00, 0xA,+0 }, // 1620: f21GM104; f21GM99; f41GM104; f41GM99; FX 4 atmosphere; Sitar
    { 0x112AA83,0x1119B91, 0x1C,0x00, 0xE,+0 }, // 1621: b41M100; f21GM100; f41GM100; FX 5 brightness; SB100.in
    { 0x001FF64,0x0F3F53E, 0xDB,0xC0, 0x4,+0 }, // 1622: b41M101; b41M102; b41P57; f21GM101; f21GM102; f32GM100; f32GM101; f32GM102; f41GM101; f41GM102; FX 5 brightness; FX 6 goblins; FX 7 echoes; belshort; cowbell.
    { 0x0AC9051,0x1F4F071, 0x1A,0x00, 0xF,+0 }, // 1623: b41M103; f21GM103; f41GM103; FX 8 sci-fi; SB103.in
    { 0x22F5570,0x31E87E0, 0x16,0x80, 0xC,+0 }, // 1624: b41M106; f21GM106; f32GM106; f41GM106; Shamisen; fstrp2.i
    { 0x0078061,0x0077062, 0x80,0x00, 0x7,+0 }, // 1625: b41M107; b41M108; b41M109; b41M93; f21GM107; f21GM108; f21GM109; f41GM107; f41GM108; f41GM109; f41GM93; Bagpipe; Kalimba; Koto; Pad 6 metallic; flute.in
    { 0x08F6EA0,0x02A65E1, 0xEC,0x00, 0xE,+0 }, // 1626: b41M110; b41M111; f21GM110; f21GM111; f41GM110; f41GM111; Fiddle; Shanai; flute2.i
    { 0x203B162,0x0046172, 0xCF,0x00, 0x2,+0 }, // 1627: f21GM114; Steel Drums
    { 0x006FA04,0x095F201, 0xD3,0x00, 0xA,+0 }, // 1628: f21GM115; Woodblock
    { 0x2129A16,0x0019A12, 0x97,0x00, 0x2,+0 }, // 1629: b41M120; f21GM120; f41GM120; Guitar FretNoise; entbell3
    { 0x0F0E029,0x031FF1E, 0x1A,0x00, 0x6,+0 }, // 1630: b41M121; b41P63; f21GM121; f21GP63; f27GP80; f27GP81; f27GP83; f27GP84; f41GM121; f41GP63; Bell Tree; Breath Noise; Jingle Bell; Mute Triangle; Open High Conga; Open Triangle; triangle
    { 0x0056581,0x0743251, 0x83,0x00, 0xA,+0 }, // 1631: b41M122; f21GM122; f32GM122; f41GM122; Seashore; synbass4
    { 0x0847162,0x0246061, 0x21,0x00, 0x8,+0 }, // 1632: f21GM124; Telephone
    { 0x0FFF000,0x02FF607, 0x00,0x00, 0x0,+0 }, // 1633: b41P35; f21GP35; f27GP27; f27GP28; f27GP29; f27GP30; f27GP31; f27GP35; f41GP35; Ac Bass Drum; bdc1.ins
    { 0x3F27026,0x0568705, 0x00,0x00, 0xE,+0 }, // 1634: f21GP36; f21GP70; Bass Drum 1; Maracas
    { 0x069F000,0x0FFF633, 0x00,0x00, 0xE,+0 }, // 1635: b41P37; f21GP37; f41GP37; Side Stick; sn1.ins
    { 0x005FC11,0x1F5DF12, 0x00,0x00, 0x1,+0 }, // 1636: f21GP38; f21GP39; f21GP40; Acoustic Snare; Electric Snare; Hand Clap
    { 0x3F6F01E,0x307F01E, 0x00,0x00, 0xE,+0 }, // 1637: b41P42; f21GP42; f27GP51; f27GP53; f27GP54; f27GP59; f41GP42; Closed High Hat; Ride Bell; Ride Cymbal 1; Ride Cymbal 2; Tambourine; hatcl2.i
    { 0x282B2A4,0x1D49703, 0x00,0x80, 0xE,+0 }, // 1638: b41P44; b41P47; b41P69; b41P70; f21GP44; f21GP47; f21GP69; f21GP71; f27GP55; f27GP71; f32GP44; f32GP46; f32GP51; f32GP52; f32GP54; f32GP69; f32GP70; f32GP71; f32GP72; f32GP73; f32GP75; f32GP80; f32GP81; f32GP82; f32GP85; f41GP44; f41GP47; f41GP69; f41GP70; f41GP71; Cabasa; Castanets; Chinese Cymbal; Claves; Long Whistle; Low-Mid Tom; Maracas; Mute Triangle; Open High Hat; Open Triangle; Pedal High Hat; Ride Cymbal 1; Shaker; Short Guiro; Short Whistle; Splash Cymbal; Tambourine; bcymbal.
    { 0x30AFF2E,0x306FF1E, 0x00,0x00, 0xE,+0 }, // 1639: b41P46; f21GP46; f41GP46; Open High Hat; hatop.in
    { 0x16FAA12,0x006FF06, 0x00,0x00, 0x0,+0 }, // 1640: b41P72; b41P73; b41P75; f21GP72; f21GP73; f21GP75; f27GP41; f27GP43; f27GP45; f27GP47; f27GP48; f27GP50; f27GP60; f27GP61; f27GP62; f27GP63; f27GP64; f27GP65; f27GP66; f27GP72; f27GP73; f27GP74; f27GP86; f27GP87; f41GP72; f41GP73; f41GP75; Claves; High Bongo; High Floor Tom; High Timbale; High Tom; High-Mid Tom; Long Guiro; Long Whistle; Low Bongo; Low Conga; Low Floor Tom; Low Timbale; Low Tom; Low-Mid Tom; Mute High Conga; Mute Surdu; Open High Conga; Open Surdu; Short Guiro; arabdrum
    { 0x040F520,0x0F7F010, 0x0D,0x89, 0xA,+0 }, // 1641: f23GM0; f23GM125; AcouGrandPiano; Helicopter
    { 0x0F466E1,0x086B0E1, 0x13,0x00, 0xC,+0 }, // 1642: f23GM24; Acoustic Guitar1
    { 0x0014171,0x03B92A1, 0x1C,0x00, 0xE,+0 }, // 1643: f23GM25; f23GM64; Acoustic Guitar2; Soprano Sax
    { 0x0064131,0x03792A1, 0x1A,0x80, 0xC,+0 }, // 1644: f23GM26; f23GM27; f23GM68; Electric Guitar1; Electric Guitar2; Oboe
    { 0x175A563,0x045A421, 0x0F,0x8D, 0x0,+0 }, // 1645: f23GM30; Distorton Guitar
    { 0x1F07151,0x1856092, 0x91,0x80, 0xA,+0 }, // 1646: f23GM48; String Ensemble1
    { 0x00FF071,0x15F63B2, 0x8D,0x80, 0xA,+0 }, // 1647: f23GM50; Synth Strings 1
    { 0x175F502,0x0358501, 0x1A,0x88, 0x0,+0 }, // 1648: f23GM51; SynthStrings 2
    { 0x040F520,0x0F7F010, 0x0D,0x90, 0xA,+0 }, // 1649: f23GM65; Alto Sax
    { 0x0A4F3F0,0x1F5F460, 0x00,0x07, 0x8,+0 }, // 1650: f23GM122; f23GM66; Seashore; Tenor Sax
    { 0x0051F21,0x00A7121, 0x98,0x00, 0x2,+0 }, // 1651: f23GM71; Clarinet
    { 0x03FFA10,0x064F210, 0x86,0x0C, 0xE,+0 }, // 1652: f23GM72; Piccolo
    { 0x0013171,0x03BF2A1, 0x1C,0x00, 0xE,+0 }, // 1653: f23GM76; Bottle Blow
    { 0x0754231,0x0F590A1, 0x98,0x80, 0xC,+0 }, // 1654: f23GM77; Shakuhachi
    { 0x0044131,0x034F2A1, 0x1A,0x80, 0xC,+0 }, // 1655: f23GM80; Lead 1 squareea
    { 0x0289130,0x048C131, 0x58,0x0E, 0xE,+0 }, // 1656: f23GM81; Lead 2 sawtooth
    { 0x0F463E0,0x08670E1, 0x1E,0x00, 0xC,+0 }, // 1657: f23GM86; Lead 7 fifths
    { 0x0175331,0x03B92A1, 0x18,0x80, 0xC,+0 }, // 1658: f23GM88; Pad 1 new age
    { 0x00B5131,0x03BA2A1, 0x1C,0x40, 0xE,+0 }, // 1659: f23GM91; Pad 4 choir
    { 0x03A4331,0x00AAA21, 0x1C,0x00, 0xC,+0 }, // 1660: f23GM92; f23GM93; Pad 5 bowedpad; Pad 6 metallic
    { 0x1FAF000,0x1FAF211, 0x02,0x85, 0x6,+0 }, // 1661: f23GM94; Pad 7 halo
    { 0x1A57121,0x0958121, 0x17,0x00, 0xE,+0 }, // 1662: f23GM105; f23GM95; Banjo; Pad 8 sweep
    { 0x054F606,0x0B3F241, 0x73,0x0E, 0x0,+0 }, // 1663: f23GM97; FX 2 soundtrack
    { 0x055F718,0x0D5E521, 0x23,0x0E, 0x0,+0 }, // 1664: f23GM104; f23GM98; FX 3 crystal; Sitar
    { 0x0A21B14,0x0A4A0F0, 0x7F,0x7F, 0x2,+0 }, // 1665: f23GM99; FX 4 atmosphere
    { 0x05285E1,0x05662E1, 0x18,0x00, 0x0,+0 }, // 1666: f23GM100; FX 5 brightness
    { 0x3F0FB02,0x006F3C2, 0x00,0x0D, 0x0,+0 }, // 1667: f23GM103; FX 8 sci-fi
    { 0x2448711,0x0B68041, 0x00,0x84, 0x0,+0 }, // 1668: f23GM107; f23GM111; Koto; Shanai
    { 0x00FBF0C,0x004F001, 0x07,0x0A, 0x0,+0 }, // 1669: f23GM112; Tinkle Bell
    { 0x0F9F913,0x0047310, 0x86,0x06, 0x0,+0 }, // 1670: f23GM116; Taiko Drum
    { 0x03FFA10,0x064F210, 0x86,0x06, 0xE,+0 }, // 1671: f23GM117; Melodic Tom
    { 0x1F0F001,0x136F7E4, 0x00,0x0A, 0x0,+0 }, // 1672: f23GM119; Reverse Cymbal
    { 0x277F810,0x006F311, 0x44,0x07, 0x8,+0 }, // 1673: f23GP36; Bass Drum 1
    { 0x200A01E,0x0FFF810, 0x00,0x0E, 0xE,+0 }, // 1674: f23GP37; Side Stick
    { 0x018BF20,0x066F800, 0x00,0x11, 0xE,+0 }, // 1675: f23GP38; f23GP40; f23GP53; f23GP55; f23GP67; Acoustic Snare; Electric Snare; High Agogo; Ride Bell; Splash Cymbal
    { 0x0FFF902,0x0FFF811, 0x19,0x06, 0x0,+0 }, // 1676: f23GP39; Hand Clap
    { 0x215CF3E,0x0F9D92E, 0x00,0x11, 0xE,+0 }, // 1677: f23GP42; Closed High Hat
    { 0x2A0B26E,0x2D4960E, 0x00,0x00, 0xE,+0 }, // 1678: f23GP49; Crash Cymbal 1
    { 0x2E0136E,0x1D4A502, 0x00,0x00, 0x0,+0 }, // 1679: f23GP51; Ride Cymbal 1
    { 0x025F522,0x005EF24, 0x95,0x9A, 0xE,+0 }, // 1680: f24GM48; String Ensemble1
    { 0x004EF26,0x0065F24, 0xA1,0x07, 0xE,+0 }, // 1681: f24GM48; String Ensemble1
    { 0x1047B20,0x072F521, 0x4B,0x00, 0xE,+0 }, // 1682: f24GM65; Alto Sax
    { 0x019992F,0x0BFFAA2, 0x00,0x22, 0xE,+0 }, // 1683: f24GM74; Recorder
    { 0x015FAA1,0x00B7F21, 0x55,0x08, 0xE,+0 }, // 1684: f24GM74; Recorder
    { 0x0137221,0x0B26425, 0x94,0x3E, 0xC,+0 }, // 1685: f24GM88; Pad 1 new age
    { 0x0739321,0x0099DA1, 0x38,0x04, 0xC,+0 }, // 1686: f24GM88; Pad 1 new age
    { 0x0298421,0x0CFF828, 0x9C,0xB2, 0xE,+0 }, // 1687: f24GM91; Pad 4 choir
    { 0x0187521,0x00A9F21, 0x22,0x07, 0xE,+0 }, // 1688: f24GM91; Pad 4 choir
    { 0x0F3F211,0x034F2E1, 0x0F,0x00, 0xA,+0 }, // 1689: f25GM1; BrightAcouGrand
    { 0x1039761,0x004C770, 0x41,0x00, 0x3,+0 }, // 1690: f25GM12; f25GM13; f25GM14; Marimba; Tubular Bells; Xylophone
    { 0x00221C1,0x014B421, 0x1A,0x00, 0xE,+0 }, // 1691: f25GM33; Electric Bass 1
    { 0x001F2F1,0x02562E1, 0xCE,0x40, 0x6,+0 }, // 1692: f25GM34; Electric Bass 2
    { 0x212F1C2,0x054F743, 0x25,0x03, 0xE,+0 }, // 1693: f25GM103; f25GM38; FX 8 sci-fi; Synth Bass 1
    { 0x2017230,0x2269420, 0x1C,0x00, 0xE,+0 }, // 1694: f25GM48; String Ensemble1
    { 0x021A161,0x116C2A1, 0x92,0x40, 0x6,+0 }, // 1695: f25GM49; String Ensemble2
    { 0x046A502,0x044F901, 0x64,0x80, 0x0,+0 }, // 1696: f25GM58; Tuba
    { 0x175F403,0x0F4F301, 0x31,0x83, 0xE,+0 }, // 1697: f25GM59; f25GM60; French Horn; Muted Trumpet
    { 0x0858300,0x0C872A0, 0x2A,0x80, 0x6,+0 }, // 1698: f25GM70; f25GM71; Bassoon; Clarinet
    { 0x0437721,0x006A5E1, 0x25,0x80, 0x8,+0 }, // 1699: f25GM72; f25GM74; Piccolo; Recorder
    { 0x0177423,0x017C563, 0x83,0x8D, 0x7,+0 }, // 1700: f25GM73; Flute
    { 0x0187132,0x038B2A1, 0x9A,0x82, 0xC,+0 }, // 1701: f25GM82; f25GM83; Lead 3 calliope; Lead 4 chiff
    { 0x0065231,0x037F2A1, 0x1B,0x80, 0xE,+0 }, // 1702: f25GM89; Pad 2 warm
    { 0x060F207,0x072F212, 0x13,0x00, 0x8,+0 }, // 1703: f25GM102; FX 7 echoes
    { 0x036BA02,0x015F901, 0x0A,0x00, 0x4,+0 }, // 1704: f25GM104; Sitar
    { 0x024F621,0x014C421, 0x13,0x80, 0x0,+0 }, // 1705: f25GM105; Banjo
    { 0x025F521,0x015C521, 0x17,0x80, 0x0,+0 }, // 1706: f25GM106; Shamisen
    { 0x02C6621,0x014A521, 0x17,0x80, 0x0,+0 }, // 1707: f25GM107; Koto
    { 0x0E6E800,0x0F6A300, 0x0D,0x00, 0x6,+0 }, // 1708: f25GM110; Fiddle
    { 0x064E400,0x074A400, 0x00,0x00, 0x7,+0 }, // 1709: f25GM111; Shanai
    { 0x2F0F009,0x047F920, 0x0D,0x00, 0xE,+0 }, // 1710: f25GM114; f25GP38; f25GP39; f25GP40; Acoustic Snare; Electric Snare; Hand Clap; Steel Drums
    { 0x0F6E901,0x006D600, 0x15,0x00, 0xE,+0 }, // 1711: f25GM117; f47GM116; f47GP86; f47GP87; Melodic Tom; Mute Surdu; Open Surdu; Taiko Drum
    { 0x0F0F280,0x0F4F480, 0x00,0x00, 0x4,+0 }, // 1712: f25GM120; Guitar FretNoise
    { 0x003F1C0,0x00110BE, 0x4F,0x0C, 0x2,+0 }, // 1713: f25GM123; Bird Tweet
    { 0x202FF8E,0x3F6F601, 0x00,0x00, 0x8,+0 }, // 1714: f25GM124; Telephone
    { 0x202FF8E,0x3F7F701, 0x00,0x00, 0x8,+0 }, // 1715: f25GM126; f25GP54; Applause/Noise; Tambourine
    { 0x1E26301,0x01E8821, 0x46,0x00, 0x6,+0 }, // 1716: f26GM24; Acoustic Guitar1
    { 0x053F101,0x0F3F211, 0x4F,0x80, 0x4,+0 }, // 1717: f27GM0; f27GM1; f27GM2; f27GM3; f27GM4; f27GM5; f27GM84; AcouGrandPiano; BrightAcouGrand; Chorused Piano; ElecGrandPiano; Honky-tonkPiano; Lead 5 charang; Rhodes Piano
    { 0x1C5C202,0x104D000, 0x11,0x00, 0xC,+0 }, // 1718: f27GM7; Clavinet
    { 0x2129A16,0x0039012, 0x97,0x04, 0x2,+0 }, // 1719: f27GM112; f27GM8; Celesta; Tinkle Bell
    { 0x0F3F507,0x0F2F501, 0x19,0x00, 0xA,+0 }, // 1720: f27GM9; Glockenspiel
    { 0x2F3F507,0x0F2F501, 0x19,0x00, 0xA,+0 }, // 1721: f27GM10; Music box
    { 0x0229F16,0x032B0D2, 0x16,0x00, 0x8,+0 }, // 1722: f27GM11; Vibraphone
    { 0x025DA05,0x015F001, 0x4E,0x00, 0xA,+0 }, // 1723: f27GM12; Marimba
    { 0x025C811,0x0F2F511, 0x29,0x00, 0xC,+0 }, // 1724: f27GM13; Xylophone
    { 0x012FF54,0x0F2F051, 0x16,0x00, 0x0,+0 }, // 1725: f27GM14; f27GM98; FX 3 crystal; Tubular Bells
    { 0x212FF54,0x0F2F051, 0x16,0x00, 0x0,+0 }, // 1726: f27GM15; Dulcimer
    { 0x106DF24,0x005FF21, 0x15,0x00, 0x1,+0 }, // 1727: f27GM16; f27GM17; f27GM18; f27GM19; Church Organ; Hammond Organ; Percussive Organ; Rock Organ
    { 0x104F223,0x0045231, 0x50,0x80, 0xE,+0 }, // 1728: f27GM20; Reed Organ
    { 0x00BF223,0x00B5230, 0x4F,0x82, 0xE,+0 }, // 1729: f27GM21; f27GM23; Accordion; Tango Accordion
    { 0x2036162,0x0058172, 0x4A,0x00, 0x2,+0 }, // 1730: f27GM22; Harmonica
    { 0x01CF201,0x087F501, 0x10,0x00, 0xA,+0 }, // 1731: f27GM24; f27GM26; f27GM27; f27GM28; Acoustic Guitar1; Electric Guitar1; Electric Guitar2; Electric Guitar3
    { 0x014F201,0x084F501, 0x10,0x00, 0xA,+0 }, // 1732: f27GM25; Acoustic Guitar2
    { 0x103AF00,0x3FFF021, 0x06,0x00, 0x6,+0 }, // 1733: f27GM29; Overdrive Guitar
    { 0x025DA05,0x06A5334, 0x8E,0x00, 0xA,+0 }, // 1734: f27GM31; Guitar Harmonics
    { 0x035F813,0x004FF11, 0x12,0x03, 0x8,+0 }, // 1735: f27GM32; f27GM33; f27GM34; f27GM35; f27GM36; f27GM37; f27GM38; f27GM39; f27GM43; Acoustic Bass; Contrabass; Electric Bass 1; Electric Bass 2; Fretless Bass; Slap Bass 1; Slap Bass 2; Synth Bass 1; Synth Bass 2
    { 0x0114172,0x01562A2, 0x89,0x40, 0xA,+0 }, // 1736: f27GM40; f27GM41; f27GM42; f27GM44; f27GM48; f27GM49; Cello; String Ensemble1; String Ensemble2; Tremulo Strings; Viola; Violin
    { 0x0F9F121,0x0F6F721, 0x1C,0x00, 0xE,+0 }, // 1737: f27GM45; Pizzicato String
    { 0x075F502,0x0F3F201, 0x29,0x00, 0x0,+0 }, // 1738: f27GM46; Orchestral Harp
    { 0x005FF00,0x0F3F020, 0x18,0x00, 0x0,+0 }, // 1739: f27GM47; Timpany
    { 0x0114172,0x01562A1, 0x89,0x40, 0xA,+0 }, // 1740: f27GM50; f27GM51; Synth Strings 1; SynthStrings 2
    { 0x2A32321,0x1F34221, 0x1A,0x00, 0x8,+0 }, // 1741: f27GM52; f27GM53; f27GM54; f27GM85; Choir Aahs; Lead 6 voice; Synth Voice; Voice Oohs
    { 0x010A130,0x0337D10, 0x07,0x00, 0x0,+0 }, // 1742: f27GM55; Orchestra Hit
    { 0x00B5131,0x13BB261, 0x1C,0x00, 0xE,+0 }, // 1743: f27GM56; f27GM59; f27GM61; f27GM62; f27GM63; f27GM64; f27GM65; f27GM66; f27GM67; Alto Sax; Baritone Sax; Brass Section; Muted Trumpet; Soprano Sax; Synth Brass 1; Synth Brass 2; Tenor Sax; Trumpet
    { 0x01D5320,0x03B6261, 0x18,0x00, 0xA,+0 }, // 1744: f27GM57; Trombone
    { 0x01572A1,0x02784A1, 0x17,0x00, 0xE,+0 }, // 1745: f27GM58; Tuba
    { 0x00351B2,0x01352A2, 0x1C,0x05, 0xE,+0 }, // 1746: b41M52; b41M54; f27GM52; f32GM52; f32GM54; f41GM52; f41GM54; Choir Aahs; Synth Voice; violin.i
    { 0x05A5321,0x01A8A21, 0x9F,0x00, 0xC,+0 }, // 1747: f27GM60; f27GM69; English Horn; French Horn
    { 0x0009F71,0x0069060, 0x51,0x00, 0x0,+0 }, // 1748: f27GM70; Bassoon
    { 0x0009F71,0x0069062, 0x51,0x00, 0x0,+0 }, // 1749: f27GM71; Clarinet
    { 0x0077061,0x0077062, 0x80,0x80, 0x7,+0 }, // 1750: f27GM72; f27GM73; f27GM74; f27GM76; f27GM77; Bottle Blow; Flute; Piccolo; Recorder; Shakuhachi
    { 0x0077061,0x0077041, 0x80,0x80, 0x7,+0 }, // 1751: f27GM75; Pan Flute
    { 0x0F7F000,0x00687A2, 0x30,0x00, 0xF,+0 }, // 1752: f27GM78; f27GM79; Ocarina; Whistle
    { 0x2129A16,0x1039012, 0x97,0x04, 0x2,+0 }, // 1753: f27GM80; Lead 1 squareea
    { 0x0037165,0x0076171, 0xD2,0x00, 0x2,+0 }, // 1754: f27GM121; f27GM82; f27GM83; Breath Noise; Lead 3 calliope; Lead 4 chiff
    { 0x0011E00,0x0A11220, 0x40,0x40, 0x6,+0 }, // 1755: f27GM86; f27GM95; Lead 7 fifths; Pad 8 sweep
    { 0x0059221,0x1059421, 0x1C,0x00, 0xE,+0 }, // 1756: f27GM87; Lead 8 brass
    { 0x044FF25,0x033F324, 0x15,0x01, 0xC,+0 }, // 1757: f27GM88; Pad 1 new age
    { 0x0132F20,0x0132321, 0x0D,0x00, 0x1,+0 }, // 1758: f27GM89; Pad 2 warm
    { 0x0012E01,0x0216221, 0x40,0x40, 0x6,+0 }, // 1759: f27GM90; Pad 3 polysynth
    { 0x3134362,0x0038261, 0x2E,0x00, 0x2,+0 }, // 1760: f27GM91; Pad 4 choir
    { 0x2035FE6,0x00350E1, 0x0F,0x00, 0x3,+0 }, // 1761: f27GM92; Pad 5 bowedpad
    { 0x3034F61,0x0035061, 0x0D,0x00, 0x9,+0 }, // 1762: f27GM93; Pad 6 metallic
    { 0x1034F61,0x0035061, 0x00,0x00, 0x9,+0 }, // 1763: f27GM94; f27GM96; FX 1 rain; Pad 7 halo
    { 0x3033F60,0x0033061, 0x0D,0x00, 0x7,+0 }, // 1764: f27GM97; FX 2 soundtrack
    { 0x112FF53,0x0F1F071, 0x13,0x00, 0x0,+0 }, // 1765: f27GM99; FX 4 atmosphere
    { 0x112FFD1,0x0F1F0F1, 0x12,0x00, 0x0,+0 }, // 1766: f27GM100; FX 5 brightness
    { 0x0E11126,0x0E11120, 0xA5,0x00, 0x0,+0 }, // 1767: f27GM101; FX 6 goblins
    { 0x30244A1,0x04245E1, 0x51,0x00, 0x2,+0 }, // 1768: f27GM102; FX 7 echoes
    { 0x0E1A126,0x0E1A120, 0xA5,0x0E, 0x0,+0 }, // 1769: f27GM103; FX 8 sci-fi
    { 0x054F101,0x004F008, 0x40,0x00, 0x0,+0 }, // 1770: f27GM104; Sitar
    { 0x011A131,0x0437D16, 0x47,0x40, 0x8,+0 }, // 1771: f27GM105; Banjo
    { 0x211A131,0x0437D11, 0x14,0x00, 0x0,+0 }, // 1772: f27GM106; Shamisen
    { 0x091AB0E,0x0C3F702, 0xC0,0x00, 0xE,+0 }, // 1773: f27GM107; Koto
    { 0x02FC811,0x0F5F431, 0x2D,0x00, 0xC,+0 }, // 1774: f27GM108; Kalimba
    { 0x1176E31,0x20CAB22, 0x43,0x08, 0x2,+0 }, // 1775: f27GM109; Bagpipe
    { 0x1176E31,0x20CAB22, 0x4F,0x08, 0x2,+0 }, // 1776: f27GM110; f27GM111; Fiddle; Shanai
    { 0x002FF64,0x0F3F522, 0xDB,0x02, 0x4,+0 }, // 1777: f27GM113; Agogo Bells
    { 0x001FF63,0x0F3F534, 0xDB,0x00, 0x2,+0 }, // 1778: f27GM114; Steel Drums
    { 0x0FFFB13,0x0FFE802, 0x40,0x00, 0x8,+0 }, // 1779: f27GM115; f27GP76; f27GP77; f27GP78; f27GP79; High Wood Block; Low Wood Block; Mute Cuica; Open Cuica; Woodblock
    { 0x108FF00,0x006F000, 0x00,0x00, 0x0,+0 }, // 1780: f27GM116; f27GM117; f27GM118; Melodic Tom; Synth Drum; Taiko Drum
    { 0x0F1100E,0x0F61800, 0x00,0x00, 0xE,+0 }, // 1781: f27GM119; Reverse Cymbal
    { 0x1F18F2A,0x1F63816, 0x00,0x00, 0x8,+0 }, // 1782: f27GM120; Guitar FretNoise
    { 0x0F0102E,0x2821020, 0x00,0x00, 0xE,+0 }, // 1783: f27GM122; Seashore
    { 0x201EFEE,0x0069FEE, 0x10,0x04, 0x6,+0 }, // 1784: f27GM123; Bird Tweet
    { 0x201EFEE,0x0069FEE, 0x01,0x04, 0x6,+0 }, // 1785: f27GM124; Telephone
    { 0x001EFEE,0x0069FE0, 0x01,0x04, 0x6,+0 }, // 1786: f27GM125; Helicopter
    { 0x001F02E,0x0064820, 0x00,0x00, 0xE,+0 }, // 1787: f27GM126; Applause/Noise
    { 0x3EFF71C,0x08FFD0E, 0x00,0x00, 0xF,+0 }, // 1788: f27GP42; Closed High Hat
    { 0x202FF0E,0x103FF1E, 0x00,0x80, 0xE,+0 }, // 1789: f27GP44; f27GP46; Open High Hat; Pedal High Hat
    { 0x202BF8E,0x2049F0E, 0x00,0x00, 0xE,+0 }, // 1790: f27GP49; f27GP52; f27GP57; Chinese Cymbal; Crash Cymbal 1; Crash Cymbal 2
    { 0x003FF64,0x0F6F73E, 0xDB,0x00, 0x4,+0 }, // 1791: f27GP56; Cow Bell
    { 0x100F300,0x054F600, 0x00,0x00, 0xC,+0 }, // 1792: f27GP58; Vibraslap
    { 0x2F3F40C,0x3D66E0E, 0x00,0x00, 0xE,+0 }, // 1793: f27GP69; f27GP70; f27GP82; Cabasa; Maracas; Shaker
    { 0x0F7F241,0x0F7F281, 0x12,0x00, 0x6,+0 }, // 1794: f29GM7; f30GM7; Clavinet
    { 0x10BD0E0,0x109E0A4, 0x80,0x8E, 0x1,+0 }, // 1795: f29GM14; Tubular Bells
    { 0x0F4F60C,0x0F5F341, 0x5C,0x00, 0x0,+0 }, // 1796: f29GM22; f29GM23; f30GM22; f30GM23; Harmonica; Tango Accordion
    { 0x1557261,0x0187121, 0x86,0x83, 0x0,+0 }, // 1797: f29GM24; f29GM25; f29GM27; f30GM24; f30GM25; f30GM27; Acoustic Guitar1; Acoustic Guitar2; Electric Guitar2
    { 0x09612F3,0x10430B1, 0x45,0x86, 0x1,+0 }, // 1798: f29GM33; Electric Bass 1
    { 0x0F6F358,0x0F6F241, 0x62,0x00, 0x0,+0 }, // 1799: f29GM40; f29GM98; f30GM40; f30GM98; FX 3 crystal; Violin
    { 0x204F061,0x2055020, 0x9D,0x83, 0xC,+0 }, // 1800: f29GM50; Synth Strings 1
    { 0x236F312,0x2D7B300, 0x2A,0x00, 0x0,+0 }, // 1801: f29GM59; Muted Trumpet
    { 0x143F701,0x1E4F3A2, 0x00,0x00, 0x8,+0 }, // 1802: f29GM61; Brass Section
    { 0x35B8721,0x00A6021, 0x99,0x00, 0xE,+0 }, // 1803: f29GM76; Bottle Blow
    { 0x0F3D385,0x0F3A341, 0x59,0x80, 0xC,+0 }, // 1804: f29GM102; f30GM102; FX 7 echoes
    { 0x125FF10,0x015F711, 0x56,0x00, 0xE,+0 }, // 1805: f29GM112; Tinkle Bell
    { 0x04AFA02,0x074F490, 0x16,0x01, 0xE,+0 }, // 1806: f29GM117; Melodic Tom
    { 0x045F668,0x0289E87, 0x00,0x01, 0x6,+0 }, // 1807: f29GP54; Tambourine
    { 0x164F923,0x177F607, 0x95,0x00, 0xE,+0 }, // 1808: f29GP66; Low Timbale
    { 0x0E7F21C,0x0B8F201, 0x6F,0x80, 0xC,+12 }, // 1809: f31GM4; Rhodes Piano
    { 0x0E2CE02,0x4E2F402, 0x25,0x00, 0x0,+0 }, // 1810: f31GM8; Celesta
    { 0x0E2F507,0x0E2F341, 0xA1,0x00, 0x0,+0 }, // 1811: f31GM8; Celesta
    { 0x2E5F5D9,0x0E5F251, 0x22,0x00, 0x8,+0 }, // 1812: f31GM11; Vibraphone
    { 0x0E1F111,0x0E1F251, 0x10,0x08, 0x9,+0 }, // 1813: f31GM11; Vibraphone
    { 0x4B1F0C9,0x0B2F251, 0x98,0x01, 0x8,+0 }, // 1814: f31GM14; Tubular Bells
    { 0x082F311,0x0E3F311, 0x44,0x80, 0x9,+0 }, // 1815: f31GM14; Tubular Bells
    { 0x0828523,0x0728212, 0xB3,0xA7, 0xE,+0 }, // 1816: f31GM46; Orchestral Harp
    { 0x0728201,0x0328411, 0x27,0x00, 0xE,+0 }, // 1817: f31GM46; Orchestral Harp
    { 0x4E5F111,0x4E5F312, 0xA1,0x40, 0x4,-12 }, // 1818: f31GM47; Timpany
    { 0x0E5F111,0x0E6F111, 0x89,0x00, 0x5,+0 }, // 1819: f31GM47; Timpany
    { 0x5047130,0x01474A0, 0x99,0x01, 0xE,+12 }, // 1820: f31GM48; f31GM49; String Ensemble1; String Ensemble2
    { 0x1147561,0x0147522, 0x88,0x00, 0xF,+0 }, // 1821: f31GM48; f31GM49; f31GM50; String Ensemble1; String Ensemble2; Synth Strings 1
    { 0x5047130,0x01474A0, 0x99,0x01, 0xE,+0 }, // 1822: f31GM50; Synth Strings 1
    { 0x0141161,0x0165561, 0x17,0x00, 0xC,+12 }, // 1823: f31GM60; French Horn
    { 0x7217230,0x604BF31, 0x1B,0x03, 0xC,+0 }, // 1824: f31GM61; Brass Section
    { 0x0357A31,0x03A7A31, 0x1D,0x09, 0xD,+0 }, // 1825: f31GM61; Brass Section
    { 0x06599E1,0x0154825, 0x80,0x85, 0x8,+0 }, // 1826: f31GM68; Oboe
    { 0x015AA62,0x0058F21, 0x94,0x80, 0x9,+0 }, // 1827: f31GM68; Oboe
    { 0x025C9A4,0x0056F21, 0xA2,0x80, 0xC,+0 }, // 1828: f31GM74; Recorder
    { 0x015CAA2,0x0056F21, 0xAA,0x00, 0xD,+0 }, // 1829: f31GM74; Recorder
    { 0x07E0824,0x0E4E383, 0x80,0x40, 0xA,+24 }, // 1830: f31GM88; Pad 1 new age
    { 0x0E6F314,0x0E6F281, 0x63,0x00, 0xB,+0 }, // 1831: f31GM88; Pad 1 new age
    { 0x205FC00,0x017FA00, 0x40,0x00, 0xE,+0 }, // 1832: f31GP40; Electric Snare
    { 0x007FC00,0x638F801, 0x00,0x80, 0xF,+0 }, // 1833: f31GP40; Electric Snare
    { 0x0038165,0x005F172, 0xD2,0x80, 0x2,+0 }, // 1834: f32GM10; f32GM9; Glockenspiel; Music box
    { 0x0F0FB3E,0x09BA071, 0x29,0x40, 0x0,+0 }, // 1835: f32GM11; Vibraphone
    { 0x0038165,0x005F171, 0xD2,0x40, 0x2,+0 }, // 1836: f32GM12; f32GM13; f32GM14; Marimba; Tubular Bells; Xylophone
    { 0x002A4B4,0x04245D7, 0x47,0x40, 0x6,+0 }, // 1837: f32GM32; Acoustic Bass
    { 0x0022A55,0x0F34212, 0x97,0x80, 0x0,+0 }, // 1838: f32GM34; f41GM34; f54GM80; Electric Bass 2; Lead 1 squareea
    { 0x001EF8F,0x0F19801, 0x81,0x00, 0x4,+0 }, // 1839: b41M35; f32GM35; f41GM35; Fretless Bass; tincan1.
    { 0x0176EB1,0x00E8B22, 0xC5,0x05, 0x2,+0 }, // 1840: b41M42; f32GM42; f47GM68; Cello; Oboe; oboe1.in
    { 0x0427887,0x0548594, 0x4D,0x00, 0xA,+0 }, // 1841: b41M46; f32GM46; f41GM46; Orchestral Harp; javaican
    { 0x01171B1,0x1154261, 0x8B,0x40, 0x6,+0 }, // 1842: f32GM48; f32GM50; f53GM73; Flute; String Ensemble1; Synth Strings 1
    { 0x08F6EE0,0x02A6561, 0xEC,0x00, 0xE,+0 }, // 1843: f32GM110; f32GM111; f32GM76; f32GM77; f47GM78; Bottle Blow; Fiddle; Shakuhachi; Shanai; Whistle
    { 0x0667190,0x08B5250, 0x92,0x00, 0xE,+0 }, // 1844: f32GM81; Lead 2 sawtooth
    { 0x00B4131,0x03B9261, 0x1C,0x80, 0xC,+0 }, // 1845: f32GM88; f32GM89; f41GM89; Pad 1 new age; Pad 2 warm
    { 0x01D5321,0x03B5261, 0x1C,0x80, 0xC,+0 }, // 1846: b41M90; f32GM90; f37GM57; f41GM90; Pad 3 polysynth; Trombone; tromb2.i
    { 0x01F41B1,0x03B9261, 0x1C,0x80, 0xE,+0 }, // 1847: b41M91; f32GM91; f41GM91; Pad 4 choir; tromb1.i
    { 0x0AE71A1,0x02E81A0, 0x1C,0x00, 0xE,+0 }, // 1848: f32GM96; FX 1 rain
    { 0x054F606,0x0B3F281, 0x73,0x03, 0x0,+0 }, // 1849: f32GM97; FX 2 soundtrack
    { 0x0177421,0x01765A2, 0x83,0x8D, 0x7,+0 }, // 1850: f32GM107; f32GM108; f32GM109; f47GM72; Bagpipe; Kalimba; Koto; Piccolo
    { 0x0F3F8E2,0x0F3F7B0, 0x86,0x40, 0x4,+0 }, // 1851: f32GM120; Guitar FretNoise
    { 0x0031801,0x090F6B4, 0x80,0xC1, 0xE,+0 }, // 1852: f32GM127; Gunshot
    { 0x282B2A4,0x1DA9803, 0x00,0x93, 0xE,+0 }, // 1853: f32GP42; Closed High Hat
    { 0x0A0B2A4,0x1D69603, 0x02,0x80, 0xE,+0 }, // 1854: f32GP49; f32GP57; f47GP30; Crash Cymbal 1; Crash Cymbal 2
    { 0x215BFD1,0x20473C1, 0x9C,0x00, 0x4,+0 }, // 1855: f34GM74; Recorder
    { 0x177F810,0x008F711, 0x91,0x00, 0x6,+0 }, // 1856: f34GP0
    { 0x277F810,0x108F311, 0xF9,0xC0, 0x6,+0 }, // 1857: f34GP2
    { 0x29BF300,0x008F311, 0x0C,0x00, 0xE,+0 }, // 1858: f34GP3
    { 0x27AFB12,0x047F611, 0x40,0x00, 0x6,+0 }, // 1859: f34GP4; f34GP5
    { 0x25DFB14,0x058F611, 0x80,0x00, 0x8,+0 }, // 1860: f34GP10; f34GP6
    { 0x12AF900,0x22BFA01, 0x02,0x00, 0x5,+0 }, // 1861: f34GP7; f34GP8
    { 0x28268D1,0x10563D0, 0x42,0x00, 0xA,+0 }, // 1862: f34GP9
    { 0x317B142,0x317B101, 0x93,0x00, 0x3,+0 }, // 1863: f34GP11
    { 0x317B242,0x317B201, 0x93,0x00, 0x3,+0 }, // 1864: f34GP12
    { 0x2BAE610,0x005EA10, 0x3F,0x3F, 0x0,+0 }, // 1865: f34GP13; f34GP15
    { 0x2BAE610,0x005EA10, 0x04,0x00, 0x0,+0 }, // 1866: f34GP14
    { 0x053B101,0x074C211, 0x4F,0x00, 0x6,+0 }, // 1867: f35GM0; f47GM0; AcouGrandPiano
    { 0x011F111,0x0B3F101, 0x4A,0x80, 0x6,+0 }, // 1868: f35GM1; BrightAcouGrand
    { 0x1FAF000,0x1FAF211, 0x02,0x80, 0x6,+0 }, // 1869: f35GM6; Harpsichord
    { 0x001F701,0x0B7F407, 0x0D,0x06, 0xA,+0 }, // 1870: f35GM7; Clavinet
    { 0x032F607,0x012F511, 0x97,0x80, 0x2,+0 }, // 1871: f35GM9; Glockenspiel
    { 0x0E3F318,0x093F241, 0x62,0x00, 0x0,+0 }, // 1872: f35GM11; Vibraphone
    { 0x025DA05,0x015F901, 0x4E,0x00, 0xA,+0 }, // 1873: f35GM12; Marimba
    { 0x1558403,0x005D341, 0x49,0x80, 0x4,+0 }, // 1874: f35GM15; Dulcimer
    { 0x01FF003,0x012F001, 0x5B,0x92, 0xA,+0 }, // 1875: f35GM18; Rock Organ
    { 0x01FF003,0x014F001, 0x5B,0x88, 0xA,+0 }, // 1876: f35GM19; Church Organ
    { 0x01FF2A0,0x07CF521, 0x11,0x00, 0xA,+0 }, // 1877: f35GM25; Acoustic Guitar2
    { 0x122F603,0x0F4F321, 0x87,0x80, 0x6,+0 }, // 1878: f35GM27; Electric Guitar2
    { 0x0442009,0x0F4D144, 0xA1,0x80, 0x8,+0 }, // 1879: f35GM31; Guitar Harmonics
    { 0x066C101,0x066A201, 0x9A,0x40, 0xA,+0 }, // 1880: f35GM32; Acoustic Bass
    { 0x08AE220,0x0A8E420, 0x11,0x00, 0xA,+0 }, // 1881: f35GM33; Electric Bass 1
    { 0x0236321,0x0266421, 0x97,0x00, 0x0,+0 }, // 1882: f35GM35; Fretless Bass
    { 0x111C031,0x1157221, 0x20,0x06, 0xC,+0 }, // 1883: f35GM41; Viola
    { 0x1107421,0x0165223, 0x0C,0x08, 0x2,+0 }, // 1884: f35GM42; Cello
    { 0x1DBB891,0x1567551, 0x17,0x00, 0xC,+0 }, // 1885: f35GM45; Pizzicato String
    { 0x075C502,0x0F3C201, 0x29,0x83, 0x0,+0 }, // 1886: f35GM46; Orchestral Harp
    { 0x0EFE800,0x0FFA401, 0x0D,0x00, 0x6,+0 }, // 1887: f35GM47; Timpany
    { 0x0117171,0x11772A1, 0x8B,0x40, 0x6,+0 }, // 1888: f35GM48; String Ensemble1
    { 0x111F0F1,0x1151121, 0x95,0x00, 0x0,+0 }, // 1889: f35GM49; String Ensemble2
    { 0x111C031,0x1159221, 0x20,0x06, 0xC,+0 }, // 1890: f35GM50; Synth Strings 1
    { 0x111C071,0x1159221, 0x20,0x08, 0xC,+0 }, // 1891: f35GM51; SynthStrings 2
    { 0x0C57461,0x165B220, 0x0F,0x08, 0xA,+0 }, // 1892: f35GM55; Orchestra Hit
    { 0x0646300,0x0757211, 0x1C,0x00, 0xE,+0 }, // 1893: f35GM58; f47GM58; Tuba
    { 0x08153E1,0x0B962E1, 0x9F,0x05, 0xE,+0 }, // 1894: f35GM60; French Horn
    { 0x0AE71E1,0x09E81E1, 0x19,0x07, 0xA,+0 }, // 1895: f35GM62; Synth Brass 1
    { 0x0AE73E1,0x09881E2, 0x49,0x08, 0xC,+0 }, // 1896: f35GM63; Synth Brass 2
    { 0x0177E71,0x00E7B22, 0xC5,0x05, 0x2,+0 }, // 1897: f35GM68; Oboe
    { 0x0D761E1,0x0F793E1, 0x85,0x80, 0xB,+0 }, // 1898: f35GM77; Shakuhachi
    { 0x20FF2D0,0x08562C1, 0xEB,0x06, 0x0,+0 }, // 1899: f35GM78; Whistle
    { 0x144F221,0x0439422, 0x8A,0x40, 0x0,+0 }, // 1900: f35GM90; Pad 3 polysynth
    { 0x1B1F2DE,0x0B281D1, 0x57,0x0A, 0xE,+0 }, // 1901: f35GM96; FX 1 rain
    { 0x05312C4,0x07212F1, 0x17,0x00, 0xA,+0 }, // 1902: f35GM97; FX 2 soundtrack
    { 0x1F6FB34,0x0439471, 0x83,0x00, 0xC,+0 }, // 1903: f35GM99; FX 4 atmosphere
    { 0x011A131,0x0437D16, 0x87,0x80, 0x8,+0 }, // 1904: f35GM105; Banjo
    { 0x141FA11,0x2F5F411, 0x06,0x00, 0x4,+0 }, // 1905: f35GM106; Shamisen
    { 0x0F0F00E,0x0841300, 0x00,0x00, 0xE,+0 }, // 1906: f35GM122; Seashore
    { 0x061F217,0x0B4F112, 0x4F,0x0A, 0x8,+0 }, // 1907: f35GM124; Telephone
    { 0x1111EF0,0x11111E2, 0x00,0xC0, 0x8,+0 }, // 1908: f35GM125; Helicopter
    { 0x003F200,0x0FFF220, 0x80,0x00, 0xE,+0 }, // 1909: f35GM127; Gunshot
    { 0x053F101,0x1F5F718, 0x4F,0x00, 0x6,+0 }, // 1910: f35GP31; f35GP32
    { 0x20CA808,0x13FD903, 0x09,0x00, 0x0,+0 }, // 1911: f35GP33; f35GP34
    { 0x0A1B2E0,0x1D6950E, 0x84,0x00, 0xE,+0 }, // 1912: f35GP57; Crash Cymbal 2
    { 0x286F265,0x228670E, 0x00,0x00, 0xE,+0 }, // 1913: f35GP69; Cabasa
    { 0x00CFD01,0x034D600, 0x07,0x00, 0x0,+0 }, // 1914: b47P35; b47P36; f36GP35; f36GP36; Ac Bass Drum; Bass Drum 1; gpo035; gpo036
    { 0x00CF600,0x004F600, 0x00,0x00, 0x1,+0 }, // 1915: b47P35; b47P36; f36GP35; f36GP36; Ac Bass Drum; Bass Drum 1; gpo035; gpo036
    { 0x0FEF512,0x0FFF652, 0x11,0xA2, 0x6,+0 }, // 1916: b47P37; f36GP37; Side Stick; gpo037
    { 0x0FFF941,0x0FFF851, 0x0F,0x00, 0x6,+0 }, // 1917: b47P37; f36GP37; Side Stick; gpo037
    { 0x205FC80,0x017FA00, 0x00,0x00, 0xE,+0 }, // 1918: b47P38; b47P40; f36GP38; f36GP40; Acoustic Snare; Electric Snare; gpo038; gpo040
    { 0x034A501,0x602FF01, 0x00,0x00, 0x7,+0 }, // 1919: b47P41; b47P42; b47P43; b47P44; b47P45; b47P46; b47P47; b47P48; b47P49; b47P50; b47P51; b47P52; b47P53; f36GP41; f36GP42; f36GP43; f36GP44; f36GP45; f36GP46; f36GP47; f36GP48; f36GP49; f36GP50; f36GP51; f36GP52; f36GP53; f36GP59; Chinese Cymbal; Closed High Hat; Crash Cymbal 1; High Floor Tom; High Tom; High-Mid Tom; Low Floor Tom; Low Tom; Low-Mid Tom; Open High Hat; Pedal High Hat; Ride Bell; Ride Cymbal 1; Ride Cymbal 2; gpo041; gpo042; gpo043; gpo044; gpo045; gpo046; gpo047; gpo048; gpo049; gpo050; gpo051; gpo052; gpo053
    { 0x007FB00,0x004A401, 0x09,0x00, 0x7,+0 }, // 1920: b47P41; b47P42; b47P43; b47P44; b47P45; b47P46; b47P47; b47P48; b47P49; b47P50; b47P51; b47P52; b47P53; f36GP41; f36GP42; f36GP43; f36GP44; f36GP45; f36GP46; f36GP47; f36GP48; f36GP49; f36GP50; f36GP51; f36GP52; f36GP53; f36GP59; Chinese Cymbal; Closed High Hat; Crash Cymbal 1; High Floor Tom; High Tom; High-Mid Tom; Low Floor Tom; Low Tom; Low-Mid Tom; Open High Hat; Pedal High Hat; Ride Bell; Ride Cymbal 1; Ride Cymbal 2; gpo041; gpo042; gpo043; gpo044; gpo045; gpo046; gpo047; gpo048; gpo049; gpo050; gpo051; gpo052; gpo053
    { 0x004F902,0x0F69705, 0x00,0x03, 0x0,+0 }, // 1921: b47P54; f36GP54; Tambourine; gpo054
    { 0x156F284,0x100F442, 0x03,0x00, 0xE,+0 }, // 1922: b47P55; f36GP55; Splash Cymbal; gpo055
    { 0x000F34F,0x0A5F48F, 0x00,0x06, 0xE,+0 }, // 1923: b47P55; f36GP55; Splash Cymbal; gpo055
    { 0x0B6FA01,0x096C802, 0x8A,0x40, 0xE,+0 }, // 1924: b47P60; f36GP60; High Bongo; gpo060
    { 0x00CF505,0x007F501, 0xEC,0x00, 0xF,+0 }, // 1925: b47P60; f36GP60; High Bongo; gpo060
    { 0x0BFFA01,0x095C802, 0x8F,0x80, 0x6,+0 }, // 1926: b47P61; f36GP61; Low Bongo; gpo061
    { 0x00CF505,0x006F501, 0xEC,0x00, 0x7,+0 }, // 1927: b47P61; f36GP61; Low Bongo; gpo061
    { 0x08DFA01,0x0BAFA03, 0x4F,0x00, 0x6,+0 }, // 1928: b47P62; b47P86; f36GP62; f36GP86; Mute High Conga; Mute Surdu; gpo062; gpo086
    { 0x08DFA01,0x0B5F803, 0x4F,0x00, 0x6,+0 }, // 1929: b47P63; b47P87; f36GP63; f36GP87; Open High Conga; Open Surdu; gpo063; gpo087
    { 0x006FA01,0x006FA00, 0x00,0x00, 0xE,+0 }, // 1930: b47P65; f36GP65; High Timbale; gpo065
    { 0x38CF800,0x06EF600, 0x80,0x00, 0xE,+0 }, // 1931: b47P66; f36GP66; Low Timbale; gpo066
    { 0x38CF803,0x0B5F80C, 0x80,0x00, 0xE,+0 }, // 1932: b47P67; f36GP67; High Agogo; gpo067
    { 0x38CF803,0x0B5F80C, 0x83,0x00, 0xE,+0 }, // 1933: b47P68; f36GP68; Low Agogo; gpo068
    { 0x049C80F,0x2699B03, 0x40,0x00, 0xE,+0 }, // 1934: b47P70; f36GP70; Maracas; gpo070
    { 0x305AD57,0x2058D47, 0xDC,0x00, 0xE,+0 }, // 1935: b47P71; f36GP71; Short Whistle; gpo071
    { 0x304A857,0x2048847, 0xDC,0x00, 0xE,+0 }, // 1936: b47P72; f36GP72; Long Whistle; gpo072
    { 0x506FF80,0x016FF10, 0x00,0x00, 0xC,+0 }, // 1937: b47P73; b47P74; f36GP73; f36GP74; Long Guiro; Short Guiro; gpo073; gpo074
    { 0x7476601,0x0476603, 0xCD,0x40, 0x8,+0 }, // 1938: b47P78; f36GP78; Mute Cuica; gpo078
    { 0x0476601,0x0576601, 0xC0,0x00, 0x9,+0 }, // 1939: b47P78; f36GP78; Mute Cuica; gpo078
    { 0x0E56701,0x0356503, 0x11,0x24, 0xA,+0 }, // 1940: b47P79; f36GP79; Open Cuica; gpo079
    { 0x0757900,0x0057601, 0x9A,0x00, 0xB,+0 }, // 1941: b47P79; f36GP79; Open Cuica; gpo079
    { 0x0E6F622,0x0E5F923, 0x1E,0x03, 0x0,+0 }, // 1942: b47P80; f36GP80; Mute Triangle; gpo080
    { 0x0E6F924,0x0E4F623, 0x28,0x00, 0x1,+0 }, // 1943: b47P80; f36GP80; Mute Triangle; gpo080
    { 0x0E6F522,0x0E5F623, 0x1E,0x03, 0x0,+0 }, // 1944: b47P81; f36GP81; Open Triangle; gpo081
    { 0x0E6F524,0x0E4F423, 0x28,0x00, 0x1,+0 }, // 1945: b47P81; f36GP81; Open Triangle; gpo081
    { 0x0E5F108,0x0E5C302, 0x66,0x86, 0x8,+0 }, // 1946: f36GP83; Jingle Bell
    { 0x052F605,0x0D5F582, 0x69,0x47, 0x9,+0 }, // 1947: f36GP83; Jingle Bell
    { 0x131FF13,0x003FF11, 0x43,0x00, 0x6,+0 }, // 1948: f37GM24; f37GM25; Acoustic Guitar1; Acoustic Guitar2
    { 0x074A302,0x075C401, 0x9A,0x80, 0xA,+0 }, // 1949: f37GM26; Electric Guitar1
    { 0x103E702,0x005E604, 0x86,0x40, 0xB,+0 }, // 1950: f37GM26; Electric Guitar1
    { 0x0145321,0x025D221, 0x8B,0x21, 0x8,+0 }, // 1951: f37GM40; Violin
    { 0x104C3A1,0x0158221, 0x9F,0x0F, 0x8,+0 }, // 1952: f37GM40; Violin
    { 0x0119131,0x11572A1, 0x8A,0x00, 0x6,+0 }, // 1953: f37GM48; String Ensemble1
    { 0x0075131,0x0399261, 0x1D,0x80, 0xE,+0 }, // 1954: f37GM56; Trumpet
    { 0x00741B1,0x0398221, 0x1C,0x87, 0xF,+0 }, // 1955: f37GM56; Trumpet
    { 0x05A5321,0x01A6C21, 0x9F,0x80, 0xC,+0 }, // 1956: f37GM60; French Horn
    { 0x0565321,0x0277C21, 0x18,0x00, 0xD,+0 }, // 1957: f37GM60; French Horn
    { 0x0299960,0x036F823, 0xA3,0x5D, 0xA,+12 }, // 1958: f37GM68; f53GM84; Lead 5 charang; Oboe
    { 0x015FAA0,0x00B8F22, 0x90,0x08, 0xA,+0 }, // 1959: f37GM68; f53GM84; Lead 5 charang; Oboe
    { 0x22871A0,0x01A8124, 0x23,0x00, 0xA,+0 }, // 1960: f37GM69; English Horn
    { 0x2287320,0x01A8424, 0x97,0x98, 0xB,+0 }, // 1961: f37GM69; English Horn
    { 0x0068B20,0x0008F21, 0x2F,0x20, 0xE,+12 }, // 1962: f37GM70; f53GM86; Bassoon; Lead 7 fifths
    { 0x007CF20,0x0097F22, 0x5B,0x00, 0xE,+0 }, // 1963: f37GM70; Bassoon
    { 0x0277784,0x01655A1, 0x9B,0x85, 0xC,+0 }, // 1964: f37GM74; Recorder
    { 0x01566A2,0x00566A1, 0x9B,0x06, 0xD,+0 }, // 1965: f37GM74; Recorder
    { 0x04CA900,0x04FD600, 0x0B,0x00, 0x0,+0 }, // 1966: f37GP35; Ac Bass Drum
    { 0x112AA03,0x1F59011, 0x1C,0x00, 0xE,+0 }, // 1967: f41GM3; Honky-tonkPiano
    { 0x073F668,0x063F5A1, 0x1B,0x0D, 0x0,+0 }, // 1968: f41GM4; Rhodes Piano
    { 0x054F1A1,0x0F4F060, 0x54,0x00, 0x2,+0 }, // 1969: f41GM6; Harpsichord
    { 0x0038164,0x005D171, 0xD2,0x80, 0x2,+0 }, // 1970: f41GM9; Glockenspiel
    { 0x0F1FB3E,0x093A071, 0x29,0x00, 0x0,+0 }, // 1971: f41GM11; Vibraphone
    { 0x141B203,0x097F211, 0x5E,0x00, 0xA,+0 }, // 1972: b41M27; f41GM27; Electric Guitar2; nylongtr
    { 0x011F111,0x0B3F101, 0x4A,0x85, 0x6,+0 }, // 1973: b41M47; f41GM47; Timpany; csynth.i
    { 0x022FE30,0x007FB20, 0x07,0x00, 0x0,+0 }, // 1974: f41GM60; French Horn
    { 0x07D8207,0x07D8214, 0x8F,0x80, 0xC,+0 }, // 1975: b41M61; f41GM61; Brass Section; elguit3.
    { 0x0527101,0x0735012, 0x8F,0x00, 0xA,+0 }, // 1976: f41GM78; f41GM79; f41GM80; Lead 1 squareea; Ocarina; Whistle
    { 0x1249F16,0x035B012, 0x11,0x00, 0x8,+0 }, // 1977: f41GM96; FX 1 rain
    { 0x1119183,0x0F1B142, 0xD7,0x00, 0x0,+0 }, // 1978: f41GM98; FX 3 crystal
    { 0x006FA04,0x005FF01, 0xD3,0x00, 0xA,+0 }, // 1979: f41GM115; Woodblock
    { 0x050B233,0x1F5B131, 0x5A,0x00, 0x0,+0 }, // 1980: b42M0; f42GM0; AcouGrandPiano; PIANO1
    { 0x153F231,0x0F5F111, 0x49,0x03, 0x6,+0 }, // 1981: b42M1; f42GM1; BrightAcouGrand; PIANO2
    { 0x183D131,0x0F5C132, 0x95,0x03, 0xC,+0 }, // 1982: b42M2; f42GM2; ElecGrandPiano; PIANO3
    { 0x163F334,0x1F59211, 0x9B,0x00, 0x0,+0 }, // 1983: b42M3; f42GM3; HONKTONK; Honky-tonkPiano
    { 0x2B7F827,0x0F9F191, 0x28,0x00, 0x0,+0 }, // 1984: b42M4; f42GM4; EP1; Rhodes Piano
    { 0x1EEF31A,0x0F5F111, 0x2D,0x00, 0x0,+0 }, // 1985: b42M5; f42GM5; Chorused Piano; EP2
    { 0x158F235,0x1F68132, 0x95,0x02, 0xE,+0 }, // 1986: b42M6; f42GM6; HARPSIC; Harpsichord
    { 0x040C931,0x1B9C235, 0x85,0x00, 0x0,+0 }, // 1987: b42M7; f42GM7; CLAVIC; Clavinet
    { 0x064C709,0x035B201, 0x15,0x05, 0x9,+0 }, // 1988: b42M8; f42GM8; CELESTA; Celesta
    { 0x044F406,0x034F201, 0x03,0x1B, 0x1,+0 }, // 1989: f42GM9; f42GP34; Glockenspiel
    { 0x124A904,0x074F501, 0x06,0x01, 0xB,+0 }, // 1990: b42M10; f42GM10; MUSICBOX; Music box
    { 0x033F6D4,0x0E361F1, 0x00,0x00, 0x1,+0 }, // 1991: b42M11; f42GM11; VIBES; Vibraphone
    { 0x0E8F7D4,0x064A4D1, 0x00,0x00, 0x5,+0 }, // 1992: b42M12; f42GM12; MARIMBA; Marimba
    { 0x0F7F736,0x0F5B531, 0x16,0x07, 0x0,+0 }, // 1993: b42M13; f42GM13; XYLO; Xylophone
    { 0x043A203,0x074F300, 0x1B,0x00, 0xA,+0 }, // 1994: b42M14; f42GM14; TUBEBELL; Tubular Bells
    { 0x135F8C3,0x194C311, 0x8E,0x00, 0x0,+0 }, // 1995: b42M15; f42GM15; Dulcimer; SANTUR
    { 0x11BF4E2,0x10DF4E0, 0x07,0x00, 0x7,+0 }, // 1996: b42M16; f42GM16; Hammond Organ; ORGAN1
    { 0x02CF6F2,0x10BF5F0, 0x00,0x00, 0x5,+0 }, // 1997: b42M17; f42GM17; ORGAN2; Percussive Organ
    { 0x015B6F1,0x007BFF0, 0x06,0x00, 0xB,+0 }, // 1998: b42M18; f42GM18; ORGAN3; Rock Organ
    { 0x1167922,0x1086DE0, 0x03,0x00, 0x9,+0 }, // 1999: b42M19; f42GM19; Church Organ; PIPEORG
    { 0x0066331,0x1175172, 0x27,0x00, 0x0,+0 }, // 2000: b42M20; f42GM20; REEDORG; Reed Organ
    { 0x11653B4,0x1175171, 0x1D,0x00, 0xE,+0 }, // 2001: b42M21; f42GM21; ACORDIAN; Accordion
    { 0x0159725,0x1085332, 0x29,0x00, 0x0,+0 }, // 2002: b42M22; f42GM22; HARMONIC; Harmonica
    { 0x0156724,0x1065331, 0x9E,0x00, 0xE,+0 }, // 2003: b42M23; f42GM23; BANDNEON; Tango Accordion
    { 0x1B4A313,0x0F8D231, 0x27,0x00, 0x4,+0 }, // 2004: b42M24; f42GM24; Acoustic Guitar1; NYLONGT
    { 0x032F317,0x1C7E211, 0xA3,0x00, 0x0,+0 }, // 2005: b42M25; f42GM25; Acoustic Guitar2; STEELGT
    { 0x1C1D233,0x09CF131, 0x24,0x00, 0xE,+0 }, // 2006: b42M26; f42GM26; Electric Guitar1; JAZZGT
    { 0x044F831,0x1C9F232, 0x05,0x02, 0x0,+0 }, // 2007: b42M27; f42GM27; CLEANGT; Electric Guitar2
    { 0x07B9C21,0x0FB9502, 0x09,0x03, 0x6,+0 }, // 2008: b42M28; f42GM28; Electric Guitar3; MUTEGT
    { 0x1988121,0x059A121, 0x84,0x04, 0x6,+0 }, // 2009: b42M29; f42GM29; OVERDGT; Overdrive Guitar
    { 0x04378B1,0x3FC9122, 0x0C,0x03, 0x0,+0 }, // 2010: b42M30; f42GM30; DISTGT; Distorton Guitar
    { 0x08C8200,0x0ECB408, 0x0A,0x02, 0x8,+0 }, // 2011: b42M31; f42GM31; GTHARMS; Guitar Harmonics
    { 0x046AB21,0x0F79321, 0x13,0x00, 0x0,+0 }, // 2012: b42M32; f42GM32; ACOUBASS; Acoustic Bass
    { 0x032F901,0x058C122, 0x0A,0x04, 0x0,+0 }, // 2013: b42M33; f42GM33; Electric Bass 1; FINGBASS
    { 0x077FA21,0x06AC322, 0x07,0x02, 0xA,+0 }, // 2014: b42M34; f42GM34; Electric Bass 2; PICKBASS
    { 0x0577121,0x0876221, 0x17,0x00, 0xA,+0 }, // 2015: b42M35; f42GM35; FRETLESS; Fretless Bass
    { 0x178FA25,0x097F312, 0x01,0x00, 0x6,+0 }, // 2016: b42M36; f42GM36; SLAPBAS1; Slap Bass 1
    { 0x088FA21,0x097B313, 0x06,0x00, 0xC,+0 }, // 2017: f42GM37; Slap Bass 2
    { 0x17FF521,0x0CCF323, 0x09,0x04, 0x8,+0 }, // 2018: b42M38; f42GM38; SYNBASS1; Synth Bass 1
    { 0x09BA301,0x0AA9301, 0x10,0x00, 0x8,+0 }, // 2019: b42M39; f42GM39; SYNBASS2; Synth Bass 2
    { 0x129F6E2,0x10878E1, 0x19,0x00, 0xC,+0 }, // 2020: b42M40; f42GM40; VIOLIN; Violin
    { 0x129F6E2,0x10878E1, 0x1C,0x00, 0xC,+0 }, // 2021: b42M41; f42GM41; VIOLA; Viola
    { 0x1166961,0x1275461, 0x19,0x00, 0xA,+0 }, // 2022: b42M42; f42GM42; CELLO; Cello
    { 0x1318271,0x0566132, 0x18,0x00, 0xC,+0 }, // 2023: b42M43; f42GM43; CONTRAB; Contrabass
    { 0x10670E2,0x11675E1, 0x23,0x00, 0xC,+0 }, // 2024: b42M44; f42GM44; TREMSTR; Tremulo Strings
    { 0x0E68802,0x1F6F561, 0x00,0x00, 0x9,+0 }, // 2025: b42M45; f42GM45; PIZZ; Pizzicato String
    { 0x1D5F612,0x0E3F311, 0x20,0x80, 0xE,+0 }, // 2026: b42M46; f42GM46; HARP; Orchestral Harp
    { 0x1F4F461,0x0F5B500, 0x0E,0x00, 0x0,+0 }, // 2027: b42M47; f42GM47; TIMPANI; Timpany
    { 0x1049C61,0x0167121, 0x1E,0x80, 0xE,+0 }, // 2028: b42M48; f42GM48; STRINGS; String Ensemble1
    { 0x2D6C0A2,0x1553021, 0x2A,0x00, 0xE,+0 }, // 2029: b42M49; f42GM49; SLOWSTR; String Ensemble2
    { 0x1357261,0x1366261, 0x21,0x00, 0xE,+0 }, // 2030: b42M50; f42GM50; SYNSTR1; Synth Strings 1
    { 0x1237221,0x0075121, 0x1A,0x02, 0xE,+0 }, // 2031: b42M51; f42GM51; SYNSTR2; SynthStrings 2
    { 0x03197E1,0x0396261, 0x16,0x00, 0x8,+0 }, // 2032: b42M52; f42GM52; CHOIR; Choir Aahs
    { 0x0457922,0x0276621, 0xC3,0x00, 0x0,+0 }, // 2033: b42M53; f42GM53; OOHS; Voice Oohs
    { 0x1556321,0x0467321, 0xDE,0x00, 0x0,+0 }, // 2034: b42M54; f42GM54; SYNVOX; Synth Voice
    { 0x0F78642,0x1767450, 0x05,0x00, 0xB,+0 }, // 2035: b42M55; f42GM55; ORCHIT; Orchestra Hit
    { 0x0026131,0x0389261, 0x1C,0x81, 0xE,+0 }, // 2036: b42M56; f42GM56; TRUMPET; Trumpet
    { 0x0235271,0x0197161, 0x1E,0x02, 0xE,+0 }, // 2037: b42M57; f42GM57; TROMBONE; Trombone
    { 0x0167621,0x0098121, 0x1A,0x01, 0xE,+0 }, // 2038: b42M58; f42GM58; TUBA; Tuba
    { 0x22C8925,0x24B8320, 0x28,0x00, 0x6,+0 }, // 2039: b42M59; f42GM59; MUTETRP; Muted Trumpet
    { 0x0167921,0x05971A2, 0x1F,0x05, 0x8,+0 }, // 2040: b42M60; f42GM60; FRHORN; French Horn
    { 0x0168721,0x0398221, 0x19,0x03, 0xE,+0 }, // 2041: b42M61; f42GM61; BRASS1; Brass Section
    { 0x0357521,0x0178422, 0x17,0x82, 0xE,+0 }, // 2042: b42M62; f42GM62; SYNBRAS1; Synth Brass 1
    { 0x0586221,0x0167221, 0x22,0x02, 0xE,+0 }, // 2043: b42M63; f42GM63; SYNBRAS2; Synth Brass 2
    { 0x10759B1,0x00A7BA1, 0x1B,0x00, 0x0,+0 }, // 2044: b42M64; f42GM64; SOPSAX; Soprano Sax
    { 0x0049F21,0x10C8521, 0x16,0x00, 0xA,+0 }, // 2045: b42M65; f42GM65; ALTOSAX; Alto Sax
    { 0x020A821,0x10A7B23, 0x0F,0x00, 0xC,+0 }, // 2046: b42M66; f42GM66; TENSAX; Tenor Sax
    { 0x0048821,0x1187926, 0x0F,0x00, 0x8,+0 }, // 2047: b42M67; f42GM67; BARISAX; Baritone Sax
    { 0x0058F31,0x0087332, 0x18,0x01, 0x0,+0 }, // 2048: b42M68; f42GM68; OBOE; Oboe
    { 0x1378CA1,0x00A7724, 0x0A,0x04, 0x0,+0 }, // 2049: b42M69; f42GM69; ENGLHORN; English Horn
    { 0x067A831,0x0195175, 0x04,0x00, 0xA,+0 }, // 2050: b42M70; f42GM70; BASSOON; Bassoon
    { 0x12677A2,0x0097421, 0x1F,0x01, 0x0,+0 }, // 2051: b42M71; f42GM71; CLARINET; Clarinet
    { 0x194B8E1,0x0286321, 0x07,0x01, 0x0,+0 }, // 2052: b42M72; f42GM72; PICCOLO; Piccolo
    { 0x05987A1,0x00A65E1, 0x93,0x00, 0x0,+0 }, // 2053: b42M73; f42GM73; FLUTE1; Flute
    { 0x0389F22,0x0296761, 0x10,0x00, 0x0,+0 }, // 2054: b42M74; f42GM74; RECORDER; Recorder
    { 0x19A88E2,0x0096721, 0x0D,0x00, 0x0,+0 }, // 2055: b42M75; f42GM75; PANFLUTE; Pan Flute
    { 0x09498A2,0x0286A21, 0x10,0x01, 0xE,+0 }, // 2056: b42M76; f42GM76; BOTTLEB; Bottle Blow
    { 0x02686F1,0x02755F1, 0x1C,0x00, 0xE,+0 }, // 2057: b42M77; f42GM77; SHAKU; Shakuhachi
    { 0x0099FE1,0x0086FE1, 0x3F,0x00, 0x1,+0 }, // 2058: b42M78; b44P78; f42GM78; WHISTLE; WHISTLE.; Whistle
    { 0x019F7E2,0x0077A21, 0x3B,0x00, 0x0,+0 }, // 2059: b42M79; f42GM79; OCARINA; Ocarina
    { 0x00C9222,0x00DA261, 0x1E,0x06, 0xE,+0 }, // 2060: b42M80; f42GM80; Lead 1 squareea; SQUARWAV
    { 0x122F421,0x05FA321, 0x15,0x00, 0xE,+0 }, // 2061: b42M81; f42GM81; Lead 2 sawtooth; SAWWAV
    { 0x16647F2,0x02742F1, 0x20,0x00, 0x2,+0 }, // 2062: b42M82; f42GM82; Lead 3 calliope; SYNCALLI
    { 0x0288861,0x049B261, 0x19,0x05, 0xE,+0 }, // 2063: b42M83; f42GM83; CHIFLEAD; Lead 4 chiff
    { 0x01B8221,0x179B223, 0x16,0x00, 0x0,+0 }, // 2064: b42M84; f42GM84; CHARANG; Lead 5 charang
    { 0x093CA21,0x01A7A22, 0x00,0x00, 0x0,+0 }, // 2065: b42M85; f42GM85; Lead 6 voice; SOLOVOX
    { 0x1C99223,0x1288222, 0x00,0x00, 0x9,+0 }, // 2066: b42M86; f42GM86; FIFTHSAW; Lead 7 fifths
    { 0x07BF321,0x05FC322, 0x1D,0x02, 0xE,+0 }, // 2067: b42M87; f42GM87; BASSLEAD; Lead 8 brass
    { 0x12581E1,0x195C4A6, 0x00,0x86, 0x1,+0 }, // 2068: b42M88; f42GM88; FANTASIA; Pad 1 new age
    { 0x0013121,0x0154421, 0x27,0x00, 0xE,+0 }, // 2069: b42M89; f42GM89; Pad 2 warm; WARMPAD
    { 0x2358360,0x006D161, 0x14,0x00, 0xC,+0 }, // 2070: b42M90; f42GM90; POLYSYN; Pad 3 polysynth
    { 0x101D3E1,0x0378262, 0x5C,0x00, 0x0,+0 }, // 2071: b42M91; f42GM91; Pad 4 choir; SPACEVOX
    { 0x2863428,0x0354121, 0x38,0x00, 0x0,+0 }, // 2072: b42M92; f42GM92; BOWEDGLS; Pad 5 bowedpad
    { 0x1F35224,0x1F53223, 0x12,0x02, 0x4,+0 }, // 2073: b42M93; f42GM93; METALPAD; Pad 6 metallic
    { 0x0A66261,0x02661A1, 0x1D,0x00, 0xA,+0 }, // 2074: b42M94; f42GM94; HALOPAD; Pad 7 halo
    { 0x1D52222,0x1053F21, 0x0F,0x84, 0xA,+0 }, // 2075: b42M95; f42GM95; Pad 8 sweep; SWEEPPAD
    { 0x024F9E3,0x0F6D131, 0x1F,0x01, 0x0,+0 }, // 2076: b42M96; f42GM96; FX 1 rain; ICERAIN
    { 0x1554163,0x10541A2, 0x00,0x00, 0x7,+0 }, // 2077: b42M97; f42GM97; FX 2 soundtrack; SOUNDTRK
    { 0x165A7C7,0x0E4F3C1, 0x25,0x05, 0x0,+0 }, // 2078: b42M98; f42GM98; CRYSTAL; FX 3 crystal
    { 0x1B7F7E3,0x1F59261, 0x19,0x00, 0x0,+0 }, // 2079: b42M99; f42GM99; ATMOSPH; FX 4 atmosphere
    { 0x044A866,0x1E4F241, 0x9B,0x04, 0xE,+0 }, // 2080: b42M100; f42GM100; BRIGHT; FX 5 brightness
    { 0x0752261,0x0254561, 0x20,0x00, 0xC,+0 }, // 2081: b42M101; f42GM101; FX 6 goblins; GOBLIN
    { 0x084F6E1,0x036A3E1, 0x21,0x01, 0xE,+0 }, // 2082: b42M102; f42GM102; ECHODROP; FX 7 echoes
    { 0x16473E2,0x10598E1, 0x14,0x01, 0xA,+0 }, // 2083: b42M103; f42GM103; FX 8 sci-fi; STARTHEM
    { 0x0347221,0x1F6A324, 0x0B,0x02, 0x8,+0 }, // 2084: b42M104; f42GM104; SITAR; Sitar
    { 0x053F421,0x0F8F604, 0x16,0x00, 0xC,+0 }, // 2085: b42M105; f42GM105; BANJO; Banjo
    { 0x002DA21,0x0F5F335, 0x18,0x00, 0xC,+0 }, // 2086: b42M106; f42GM106; SHAMISEN; Shamisen
    { 0x063FA25,0x1E59402, 0x0F,0x00, 0x8,+0 }, // 2087: b42M107; f42GM107; KOTO; Koto
    { 0x096F932,0x0448411, 0x07,0x00, 0x0,+0 }, // 2088: b42M108; f42GM108; KALIMBA; Kalimba
    { 0x2189720,0x1188325, 0x0E,0x03, 0x8,+0 }, // 2089: b42M109; f42GM109; BAGPIPE; Bagpipe
    { 0x029F661,0x1087862, 0x18,0x01, 0x0,+0 }, // 2090: b42M110; f42GM110; FIDDLE; Fiddle
    { 0x01976E6,0x1088E61, 0x21,0x03, 0xA,+0 }, // 2091: b42M111; f42GM111; SHANNAI; Shanai
    { 0x0D4F027,0x046F205, 0x23,0x09, 0x0,+0 }, // 2092: b42M112; f42GM112; TINKLBEL; Tinkle Bell
    { 0x031F91C,0x0E89615, 0x0C,0x00, 0xE,+0 }, // 2093: f42GM113; f42GP67; f42GP68; Agogo Bells; High Agogo; Low Agogo
    { 0x2167502,0x1F6F601, 0x00,0x00, 0x7,+0 }, // 2094: b42M114; f42GM114; STEELDRM; Steel Drums
    { 0x0F2FA25,0x09AF612, 0x1B,0x00, 0x0,+0 }, // 2095: b42M115; b42P33; b42P76; b42P77; f42GM115; f42GP33; f42GP76; f42GP77; High Wood Block; Low Wood Block; WOODBLOK; Woodblock; woodblok
    { 0x093F502,0x045C600, 0x1D,0x00, 0x0,+0 }, // 2096: b42M116; b42P87; f42GM116; f42GP87; Open Surdu; TAIKO; Taiko Drum; taiko
    { 0x032F511,0x0B4F410, 0x15,0x00, 0x4,+0 }, // 2097: b42M117; f42GM117; MELOTOM; Melodic Tom
    { 0x099FA22,0x025D501, 0x06,0x00, 0x8,+0 }, // 2098: b42M118; f42GM118; SYNDRUM; Synth Drum
    { 0x0F7F521,0x0F7F521, 0x99,0x80, 0xE,+0 }, // 2099: f42GM119; Reverse Cymbal
    { 0x038B2F1,0x0488122, 0x19,0x40, 0xC,+0 }, // 2100: f42GM120; Guitar FretNoise
    { 0x016D221,0x0F8C201, 0x1D,0x00, 0xA,+0 }, // 2101: f42GM121; Breath Noise
    { 0x082D301,0x0B8D301, 0x4E,0x06, 0xA,+0 }, // 2102: f42GM122; Seashore
    { 0x0036101,0x0F86101, 0x14,0x0D, 0xC,+0 }, // 2103: f42GM123; Bird Tweet
    { 0x017F321,0x0E8F222, 0x17,0x08, 0xC,+0 }, // 2104: f42GM124; Telephone
    { 0x0CEB161,0x1BAD061, 0x13,0x40, 0xA,+0 }, // 2105: f42GM125; Helicopter
    { 0x075C130,0x0659131, 0x10,0x42, 0xA,+0 }, // 2106: f42GM126; Applause/Noise
    { 0x00F9F3E,0x0FA8730, 0x00,0x00, 0xE,+0 }, // 2107: b42P28; b42P39; f42GM127; f42GP28; f42GP39; Clap; Gunshot; Hand Clap; clap
    { 0x0977801,0x0988802, 0x00,0x00, 0x8,+0 }, // 2108: f42GP29; f42GP30
    { 0x0FBF116,0x069F911, 0x08,0x00, 0x0,+0 }, // 2109: b42P31; b42P37; b42P85; b42P86; f42GP31; f42GP37; f42GP85; f42GP86; Castanets; Mute Surdu; RimShot; Side Stick; rimShot; rimshot
    { 0x06CF800,0x04AE80E, 0x00,0x40, 0x0,+0 }, // 2110: f42GP32
    { 0x0F3F900,0x08AF701, 0x00,0x00, 0x4,+0 }, // 2111: b42P35; f42GP35; Ac Bass Drum; Kick2
    { 0x000FF24,0x0A9F702, 0x00,0x00, 0xE,+0 }, // 2112: b42P38; b42P40; f42GP38; f42GP40; Acoustic Snare; Electric Snare; Snare
    { 0x0FEF22C,0x0D8B802, 0x00,0x1A, 0x6,+0 }, // 2113: f42GP42; f42GP44; Closed High Hat; Pedal High Hat
    { 0x0F6822E,0x0F87404, 0x00,0x27, 0x4,+0 }, // 2114: f42GP46; Open High Hat
    { 0x0009F2C,0x0D4C50E, 0x00,0x05, 0xE,+0 }, // 2115: f42GP49; f42GP52; f42GP55; f42GP57; Chinese Cymbal; Crash Cymbal 1; Crash Cymbal 2; Splash Cymbal
    { 0x0009429,0x044F904, 0x10,0x04, 0xE,+0 }, // 2116: f42GP51; f42GP53; f42GP59; Ride Bell; Ride Cymbal 1; Ride Cymbal 2
    { 0x0F1F52E,0x0F78706, 0x09,0x03, 0x0,+0 }, // 2117: f42GP54; Tambourine
    { 0x0A1F737,0x028F603, 0x14,0x00, 0x8,+0 }, // 2118: f42GP56; Cow Bell
    { 0x000FF80,0x0F7F500, 0x00,0x00, 0xC,+0 }, // 2119: f42GP58; Vibraslap
    { 0x0FAFA25,0x0F99903, 0xC4,0x00, 0x0,+0 }, // 2120: b42P60; b42P62; f42GP60; f42GP62; High Bongo; Mute High Conga; mutecong
    { 0x0FAFB21,0x0F7A802, 0x03,0x00, 0x0,+0 }, // 2121: f42GP61; Low Bongo
    { 0x0FAF924,0x0F6A603, 0x18,0x00, 0xE,+0 }, // 2122: f42GP63; f42GP64; Low Conga; Open High Conga
    { 0x0F5F505,0x036F603, 0x14,0x00, 0x6,+0 }, // 2123: f42GP65; f42GP66; High Timbale; Low Timbale
    { 0x001FF0E,0x077790E, 0x00,0x02, 0xE,+0 }, // 2124: f42GP70; Maracas
    { 0x007AF20,0x02BA50E, 0x15,0x00, 0x4,+0 }, // 2125: f42GP71; Short Whistle
    { 0x007BF20,0x03B930E, 0x18,0x00, 0x0,+0 }, // 2126: f42GP72; Long Whistle
    { 0x0F7F020,0x03B8908, 0x00,0x01, 0xA,+0 }, // 2127: f42GP73; Short Guiro
    { 0x0FAF320,0x02B5308, 0x00,0x0A, 0x8,+0 }, // 2128: f42GP74; Long Guiro
    { 0x09AF815,0x089F613, 0x21,0x10, 0x8,+0 }, // 2129: f42GP75; Claves
    { 0x0075F20,0x04B8708, 0x01,0x00, 0x0,+0 }, // 2130: f42GP78; Mute Cuica
    { 0x0F75725,0x0677803, 0x12,0x00, 0x0,+0 }, // 2131: f42GP79; Open Cuica
    { 0x0F0F122,0x0FCF827, 0x2F,0x02, 0x6,+0 }, // 2132: b42P80; f42GP80; Mute Triangle; mutringl
    { 0x0F0F126,0x0F5F527, 0x97,0xA1, 0x4,+0 }, // 2133: f42GP81; f42GP83; f42GP84; Bell Tree; Jingle Bell; Open Triangle
    { 0x054F123,0x173F231, 0x66,0x00, 0x6,+0 }, // 2134: f47GM2; ElecGrandPiano
    { 0x058F381,0x058F201, 0x63,0x80, 0x0,+0 }, // 2135: f47GM4; Rhodes Piano
    { 0x010A132,0x0337D16, 0x87,0x80, 0x8,+0 }, // 2136: f47GM6; Harpsichord
    { 0x143F523,0x204F811, 0x0E,0x00, 0x0,+0 }, // 2137: f47GM7; Clavinet
    { 0x0100133,0x0027D14, 0x87,0x80, 0x8,+0 }, // 2138: f47GM8; Celesta
    { 0x001AF64,0x062A33F, 0xDB,0xC0, 0x4,+0 }, // 2139: f47GM14; Tubular Bells
    { 0x0118171,0x1156261, 0x8B,0x40, 0x6,+0 }, // 2140: f47GM26; Electric Guitar1
    { 0x0127171,0x11652E1, 0x8B,0x40, 0x6,+0 }, // 2141: f47GM27; Electric Guitar2
    { 0x143F523,0x208F831, 0x0E,0x00, 0x0,+0 }, // 2142: f47GM28; Electric Guitar3
    { 0x0E7F301,0x078F201, 0x58,0x00, 0xA,+0 }, // 2143: f47GM32; f47GM33; Acoustic Bass; Electric Bass 1
    { 0x054C701,0x096A201, 0x4D,0x00, 0x4,+0 }, // 2144: f47GM35; Fretless Bass
    { 0x154C701,0x096A201, 0x4D,0x00, 0x4,+0 }, // 2145: f47GM36; Slap Bass 1
    { 0x0C28621,0x0BDF221, 0x16,0x00, 0x2,+0 }, // 2146: f47GM37; Slap Bass 2
    { 0x08DF520,0x08CF311, 0x49,0x00, 0xA,+12 }, // 2147: f47GM38; Synth Bass 1
    { 0x09EF520,0x05BF411, 0x90,0x00, 0xC,+12 }, // 2148: f47GM39; Synth Bass 2
    { 0x5144261,0x3344261, 0x87,0x82, 0x1,+0 }, // 2149: f47GM40; Violin
    { 0x02371A1,0x1286371, 0x4F,0x02, 0x6,+0 }, // 2150: f47GM42; Cello
    { 0x11152F0,0x12E32F1, 0xC5,0x80, 0x0,+0 }, // 2151: f47GM43; Contrabass
    { 0x01171F1,0x11542E1, 0x8B,0x40, 0x6,+0 }, // 2152: f47GM44; Tremulo Strings
    { 0x01FF201,0x088F701, 0x17,0x00, 0xA,+0 }, // 2153: f47GM45; Pizzicato String
    { 0x054C701,0x096A201, 0x8D,0x00, 0x4,-24 }, // 2154: f47GM47; f53GM118; Synth Drum; Timpany
    { 0x0117171,0x11542E1, 0x8B,0x40, 0x6,+0 }, // 2155: f47GM48; String Ensemble1
    { 0x111F0F1,0x1131121, 0x95,0x00, 0x0,+0 }, // 2156: f47GM49; String Ensemble2
    { 0x053F121,0x1743232, 0x4F,0x00, 0x6,+0 }, // 2157: f47GM50; Synth Strings 1
    { 0x0117171,0x1154261, 0x8B,0x40, 0x6,+0 }, // 2158: f47GM51; SynthStrings 2
    { 0x01271B1,0x1166261, 0x8B,0x40, 0x6,+0 }, // 2159: f47GM52; Choir Aahs
    { 0x011A1B1,0x1159261, 0x8B,0x40, 0x6,+0 }, // 2160: f47GM53; Voice Oohs
    { 0x5176261,0x3176261, 0x80,0x82, 0x5,+0 }, // 2161: f47GM54; Synth Voice
    { 0x5155261,0x3166362, 0x80,0x83, 0x5,+0 }, // 2162: f47GM55; Orchestra Hit
    { 0x0065131,0x03B9261, 0x1C,0x80, 0xE,+0 }, // 2163: f47GM56; Trumpet
    { 0x01F61B1,0x03B9261, 0x1C,0x80, 0xE,+0 }, // 2164: f47GM57; Trombone
    { 0x0276561,0x2275570, 0x83,0x03, 0xB,+0 }, // 2165: f47GM59; Muted Trumpet
    { 0x0537101,0x07C6212, 0x4E,0x00, 0xA,+0 }, // 2166: f47GM65; Alto Sax
    { 0x0658181,0x07C52B2, 0x93,0x00, 0xA,+0 }, // 2167: f47GM66; Tenor Sax
    { 0x02661B0,0x0375271, 0x96,0x00, 0xE,+12 }, // 2168: f47GM67; Baritone Sax
    { 0x0AA7724,0x0173431, 0x5B,0x00, 0xE,+0 }, // 2169: f47GM74; f54GM75; Pan Flute; Recorder
    { 0x0A6FF64,0x01424B1, 0x8A,0x00, 0xE,+0 }, // 2170: f47GM75; Pan Flute
    { 0x0A4F724,0x0132431, 0x5B,0x00, 0xE,+0 }, // 2171: f47GM76; Bottle Blow
    { 0x0384161,0x028E1A1, 0x97,0x00, 0x6,+0 }, // 2172: f47GM83; Lead 4 chiff
    { 0x01797F1,0x048F321, 0x06,0x0D, 0x8,+0 }, // 2173: f47GM84; Lead 5 charang
    { 0x054F406,0x053F281, 0x73,0x03, 0x0,+0 }, // 2174: f47GM88; Pad 1 new age
    { 0x1E31111,0x0D42101, 0x09,0x05, 0x6,+0 }, // 2175: f47GM93; Pad 6 metallic
    { 0x30217B1,0x0057321, 0x29,0x03, 0x6,+0 }, // 2176: f47GM94; Pad 7 halo
    { 0x08311E6,0x0541120, 0x11,0x00, 0x0,+0 }, // 2177: f47GM101; FX 6 goblins
    { 0x00361B1,0x0175461, 0x1F,0x01, 0xE,+0 }, // 2178: f47GM110; Fiddle
    { 0x0F00000,0x0A21B14, 0x02,0x80, 0xE,+0 }, // 2179: f47GM122; Seashore
    { 0x03FB300,0x0F0AB08, 0x80,0x00, 0xA,+0 }, // 2180: f47GP33; f47GP37; Side Stick
    { 0x1B29510,0x0069510, 0x11,0x00, 0x8,+0 }, // 2181: f47GP36; Bass Drum 1
    { 0x0F0F000,0x0B69800, 0x00,0x08, 0xE,+0 }, // 2182: f47GP38; Acoustic Snare
    { 0x0F0F009,0x0F7B720, 0x0E,0x0A, 0xE,+0 }, // 2183: f47GP39; Hand Clap
    { 0x21AF400,0x008F800, 0x00,0x08, 0xC,+0 }, // 2184: f47GP40; Electric Snare
    { 0x054C701,0x096A201, 0x8D,0x00, 0x4,+0 }, // 2185: f47GP41; f47GP42; f47GP43; f47GP44; f47GP45; f47GP46; f47GP47; f47GP48; f47GP49; f47GP50; f47GP51; f47GP52; f47GP53; Chinese Cymbal; Closed High Hat; Crash Cymbal 1; High Floor Tom; High Tom; High-Mid Tom; Low Floor Tom; Low Tom; Low-Mid Tom; Open High Hat; Pedal High Hat; Ride Bell; Ride Cymbal 1
    { 0x202FF4F,0x3F6F601, 0x00,0x0F, 0x8,+0 }, // 2186: f47GP54; Tambourine
    { 0x300EF9E,0x0D8A705, 0x80,0x00, 0xC,+0 }, // 2187: f47GP56; f47GP67; f47GP68; Cow Bell; High Agogo; Low Agogo
    { 0x0F0F006,0x035C4C4, 0x00,0x03, 0xE,+0 }, // 2188: f47GP57; Crash Cymbal 2
    { 0x210BA2F,0x2F4B40F, 0x0E,0x00, 0xE,+0 }, // 2189: f47GP59; Ride Cymbal 2
    { 0x053F101,0x0B5F700, 0x7F,0x00, 0x6,+0 }, // 2190: f47GP60; f47GP61; High Bongo; Low Bongo
    { 0x013FA43,0x096F342, 0xD6,0x80, 0xA,+0 }, // 2191: f47GP65; f47GP66; High Timbale; Low Timbale
    { 0x030F930,0x0FEF600, 0x01,0x00, 0xE,+0 }, // 2192: f47GP73; Short Guiro
    { 0x0FF0006,0x0FDF715, 0x3F,0x0D, 0x0,+0 }, // 2193: f47GP75; Claves
    { 0x0F0F006,0x0B4F600, 0x00,0x20, 0xE,+0 }, // 2194: f47GP88
    { 0x1DEB421,0x0EEF231, 0x45,0x00, 0x6,+0 }, // 2195: f48GM2; ElecGrandPiano
    { 0x0135821,0x0031531, 0x2B,0x00, 0x8,+0 }, // 2196: f48GM3; Honky-tonkPiano
    { 0x0ADF321,0x05DF321, 0x08,0x00, 0x8,+0 }, // 2197: f48GM7; Clavinet
    { 0x0EFD245,0x0EFA301, 0x4F,0x00, 0xA,+0 }, // 2198: f48GM8; Celesta
    { 0x0E7F217,0x0E7C211, 0x54,0x06, 0xA,+0 }, // 2199: f48GM9; Glockenspiel
    { 0x0C7F219,0x0D7F291, 0x2B,0x07, 0xB,+0 }, // 2200: f48GM9; Glockenspiel
    { 0x1084331,0x0084232, 0x93,0x00, 0xC,+0 }, // 2201: f48GM12; Marimba
    { 0x0084522,0x01844F1, 0x65,0x00, 0xD,+0 }, // 2202: f48GM12; Marimba
    { 0x0E8F318,0x0F8F281, 0x62,0x00, 0x0,+0 }, // 2203: f48GM13; Xylophone
    { 0x0DFD441,0x0DFC280, 0x8A,0x0C, 0x4,+0 }, // 2204: f48GM14; Tubular Bells
    { 0x0DFD345,0x0FFA381, 0x93,0x00, 0x5,+0 }, // 2205: f48GM14; Tubular Bells
    { 0x02FA2A0,0x02FA522, 0x85,0x9E, 0x7,+0 }, // 2206: f48GM16; Hammond Organ
    { 0x02FA5A2,0x02FA128, 0x83,0x95, 0x7,+0 }, // 2207: f48GM16; Hammond Organ
    { 0x03AC620,0x05AF621, 0x81,0x80, 0x7,+0 }, // 2208: f48GM17; Percussive Organ
    { 0x02CA760,0x00DAFE1, 0xC6,0x80, 0x4,+0 }, // 2209: f48GM18; Rock Organ
    { 0x0EEF121,0x17FD131, 0x00,0x00, 0x4,+0 }, // 2210: f48GM20; Reed Organ
    { 0x02FA7A3,0x00FAFE1, 0x56,0x83, 0x8,+0 }, // 2211: f48GM21; Accordion
    { 0x00FAF61,0x00FAFA2, 0x91,0x83, 0x9,+0 }, // 2212: f48GM21; Accordion
    { 0x275A421,0x1456161, 0x13,0x00, 0x4,+0 }, // 2213: f48GM27; Electric Guitar2
    { 0x4FAB913,0x0DA9102, 0x0D,0x1A, 0xA,+0 }, // 2214: f48GM29; Overdrive Guitar
    { 0x04FF923,0x2FF9122, 0xA1,0x16, 0xE,+0 }, // 2215: f48GM30; Distorton Guitar
    { 0x0BF9120,0x04F9122, 0x99,0x00, 0xE,+0 }, // 2216: f48GM30; Distorton Guitar
    { 0x0432121,0x0355222, 0x97,0x00, 0x8,+0 }, // 2217: f48GM31; Guitar Harmonics
    { 0x0AD9101,0x0CD9301, 0x53,0x00, 0x2,+0 }, // 2218: f48GM32; Acoustic Bass
    { 0x0EAF111,0x0EAF312, 0xA8,0x57, 0x4,+0 }, // 2219: f48GM33; Electric Bass 1
    { 0x0EAE111,0x0EAE111, 0x97,0x00, 0x4,+0 }, // 2220: f48GM33; Electric Bass 1
    { 0x0ECF131,0x07DF131, 0x8D,0x00, 0xA,+0 }, // 2221: f48GM34; Electric Bass 2
    { 0x02A5131,0x04A7132, 0x5B,0x00, 0xC,+0 }, // 2222: f48GM35; Fretless Bass
    { 0x04A7131,0x04A7131, 0x19,0x00, 0xD,+0 }, // 2223: f48GM35; Fretless Bass
    { 0x0AE9101,0x0CE9302, 0x93,0x00, 0x6,+0 }, // 2224: f48GM36; Slap Bass 1
    { 0x02FF120,0x3CFF220, 0x8C,0x00, 0x6,+0 }, // 2225: f48GM37; Slap Bass 2
    { 0x0EBF431,0x07AF131, 0x8B,0x00, 0xA,+0 }, // 2226: f48GM38; Synth Bass 1
    { 0x04FF220,0x35FF222, 0x94,0x00, 0x8,+0 }, // 2227: f48GM39; Synth Bass 2
    { 0x2036130,0x21754A0, 0x95,0x00, 0xA,+0 }, // 2228: f48GM41; Viola
    { 0x3107560,0x2176520, 0x89,0x00, 0xB,+0 }, // 2229: f48GM41; Viola
    { 0x513DD31,0x0385621, 0x95,0x00, 0xC,+0 }, // 2230: f48GM42; Cello
    { 0x1038D13,0x0786025, 0x95,0x89, 0xD,+0 }, // 2231: f48GM42; Cello
    { 0x121F131,0x0166FE1, 0x40,0x00, 0x2,+0 }, // 2232: f48GM44; Tremulo Strings
    { 0x1038D14,0x0266620, 0x95,0x89, 0x9,+0 }, // 2233: f48GM45; Pizzicato String
    { 0x1FFF510,0x0FFF211, 0x41,0x00, 0x2,+0 }, // 2234: f48GM47; Timpany
    { 0x1176561,0x0176521, 0x96,0x00, 0xF,+0 }, // 2235: f48GM48; String Ensemble1
    { 0x2097861,0x1095821, 0x16,0x00, 0x8,+0 }, // 2236: f48GM49; String Ensemble2
    { 0x121F131,0x0177C61, 0x40,0x00, 0x2,+0 }, // 2237: f48GM52; Choir Aahs
    { 0x6EF1F15,0x6E21115, 0xC0,0x40, 0xE,+0 }, // 2238: f48GM53; Voice Oohs
    { 0x0E21111,0x0E31111, 0x40,0x00, 0xE,+0 }, // 2239: f48GM53; Voice Oohs
    { 0x2686500,0x616C500, 0x00,0x00, 0xB,+0 }, // 2240: f48GM55; Orchestra Hit
    { 0x6DAC600,0x30E7400, 0x00,0x00, 0xB,+0 }, // 2241: f48GM55; Orchestra Hit
    { 0x01C8521,0x00C8F21, 0x92,0x01, 0xC,+0 }, // 2242: f48GM56; Trumpet
    { 0x01C8421,0x00CAF61, 0x15,0x0B, 0xD,+0 }, // 2243: f48GM56; Trumpet
    { 0x01B8521,0x00B7F21, 0x94,0x05, 0xC,+0 }, // 2244: f48GM57; Trombone
    { 0x01B8421,0x00BAF61, 0x15,0x0D, 0xD,+0 }, // 2245: f48GM57; Trombone
    { 0x0158621,0x0378221, 0x94,0x00, 0xC,+0 }, // 2246: f48GM58; Tuba
    { 0x0178521,0x0098F61, 0x92,0x00, 0xC,+0 }, // 2247: f48GM59; Muted Trumpet
    { 0x00A7321,0x00B8F21, 0x9F,0x00, 0xE,+0 }, // 2248: f48GM60; French Horn
    { 0x00A65A1,0x00B9F61, 0x9B,0x00, 0xF,+0 }, // 2249: f48GM60; French Horn
    { 0x02E7221,0x00E8F21, 0x16,0x00, 0xC,+0 }, // 2250: f48GM61; Brass Section
    { 0x0EE7521,0x03E8A21, 0x1D,0x00, 0xD,+0 }, // 2251: f48GM61; Brass Section
    { 0x0AC54A1,0x01CA661, 0x50,0x00, 0x8,+0 }, // 2252: f48GM63; Synth Brass 2
    { 0x2089331,0x00A72A1, 0x96,0x00, 0x8,+0 }, // 2253: f48GM64; Soprano Sax
    { 0x0088521,0x12A8431, 0x96,0x00, 0x9,+0 }, // 2254: f48GM64; Soprano Sax
    { 0x10A9331,0x00D72A1, 0x8E,0x00, 0x8,+0 }, // 2255: f48GM65; Alto Sax
    { 0x00AC524,0x12D6431, 0xA1,0x00, 0x9,+0 }, // 2256: f48GM65; Alto Sax
    { 0x10F9331,0x00F7271, 0x8D,0x00, 0xA,+0 }, // 2257: f48GM66; Tenor Sax
    { 0x006A524,0x11664B1, 0x9D,0x00, 0xB,+0 }, // 2258: f48GM66; Tenor Sax
    { 0x02AA961,0x036A863, 0xA3,0x52, 0x8,+0 }, // 2259: f48GM68; Oboe
    { 0x016AA61,0x00A8F61, 0x94,0x80, 0x8,+0 }, // 2260: f48GM68; Oboe
    { 0x51E7E71,0x10F8B21, 0x4D,0x00, 0x6,+0 }, // 2261: f48GM69; English Horn
    { 0x1197531,0x0196172, 0x8E,0x00, 0xA,+0 }, // 2262: f48GM70; Bassoon
    { 0x0269B32,0x0187321, 0x90,0x00, 0x4,+0 }, // 2263: f48GM71; Clarinet
    { 0x02F7721,0x02F7A73, 0x21,0x55, 0x2,+0 }, // 2264: f48GM72; Piccolo
    { 0x01F7A21,0x01F7A22, 0x93,0x00, 0x2,+0 }, // 2265: f48GM72; Piccolo
    { 0x01DAFA1,0x00D7521, 0x9C,0x00, 0x2,+0 }, // 2266: f48GM73; Flute
    { 0x011DA65,0x068A663, 0x00,0x1E, 0xC,+0 }, // 2267: f48GM75; Pan Flute
    { 0x0588861,0x01A6561, 0x8C,0x00, 0xD,+0 }, // 2268: f48GM75; Pan Flute
    { 0x1282121,0x0184161, 0x12,0x00, 0x0,+0 }, // 2269: f48GM77; Shakuhachi
    { 0x00FFF21,0x60FFF21, 0x09,0x80, 0x5,+0 }, // 2270: f48GM80; Lead 1 squareea
    { 0x3FAF100,0x3FAF111, 0x8E,0x00, 0x0,+0 }, // 2271: f48GM81; Lead 2 sawtooth
    { 0x2C686A1,0x0569321, 0x46,0x80, 0xA,+0 }, // 2272: f48GM82; Lead 3 calliope
    { 0x01B7D61,0x01B72B1, 0x40,0x23, 0xE,+0 }, // 2273: f48GM85; Lead 6 voice
    { 0x00BDFA2,0x00B7F61, 0x5D,0x80, 0xF,+0 }, // 2274: f48GM85; Lead 6 voice
    { 0x009FF20,0x40A8F61, 0x36,0x00, 0x8,+0 }, // 2275: f48GM86; Lead 7 fifths
    { 0x00FFF21,0x40D8F61, 0x27,0x00, 0x9,+0 }, // 2276: f48GM86; Lead 7 fifths
    { 0x0FCF521,0x0FDF523, 0x0F,0x00, 0xA,+0 }, // 2277: f48GM87; Lead 8 brass
    { 0x0FDF926,0x6FCF921, 0x16,0x00, 0xB,+0 }, // 2278: f48GM87; Lead 8 brass
    { 0x011A861,0x0032531, 0x1F,0x80, 0xA,+0 }, // 2279: f48GM89; Pad 2 warm
    { 0x031A101,0x0032571, 0xA1,0x00, 0xB,+0 }, // 2280: f48GM89; Pad 2 warm
    { 0x0141161,0x0175561, 0x17,0x00, 0xC,+0 }, // 2281: f48GM90; Pad 3 polysynth
    { 0x446C361,0x026C361, 0x14,0x00, 0xD,+0 }, // 2282: f48GM90; Pad 3 polysynth
    { 0x63311E1,0x0353261, 0x89,0x03, 0xA,+0 }, // 2283: f48GM92; Pad 5 bowedpad
    { 0x6E42161,0x6D53261, 0x8C,0x03, 0xB,+0 }, // 2284: f48GM92; Pad 5 bowedpad
    { 0x0336121,0x0355261, 0x8D,0x03, 0xA,+0 }, // 2285: f48GM93; Pad 6 metallic
    { 0x177A1A1,0x1471121, 0x1C,0x00, 0xB,+0 }, // 2286: f48GM93; Pad 6 metallic
    { 0x03311E1,0x0353261, 0x89,0x03, 0xA,+0 }, // 2287: f48GM94; Pad 7 halo
    { 0x0E42161,0x0D53261, 0x8C,0x03, 0xB,+0 }, // 2288: f48GM94; Pad 7 halo
    { 0x003A801,0x005A742, 0x99,0x00, 0xD,+0 }, // 2289: f48GM96; FX 1 rain
    { 0x2332121,0x0143260, 0x8C,0x97, 0x6,+0 }, // 2290: f48GM97; FX 2 soundtrack
    { 0x1041161,0x0143121, 0x0E,0x00, 0x7,+0 }, // 2291: f48GM97; FX 2 soundtrack
    { 0x056B222,0x054F261, 0x92,0x00, 0xC,+0 }, // 2292: f48GM99; FX 4 atmosphere
    { 0x04311A1,0x0741161, 0x0E,0x92, 0xA,+0 }, // 2293: f48GM101; FX 6 goblins
    { 0x0841161,0x0041DA1, 0x8E,0x80, 0xB,+0 }, // 2294: f48GM101; FX 6 goblins
    { 0x0346161,0x0055D21, 0x4C,0x80, 0x6,+0 }, // 2295: f48GM102; FX 7 echoes
    { 0x0CFF411,0x1EFF411, 0x05,0x00, 0x4,+0 }, // 2296: f48GM106; Shamisen
    { 0x035D493,0x114EB11, 0x11,0x00, 0x8,+0 }, // 2297: f48GM107; Koto
    { 0x035D453,0x116EB13, 0x11,0x0D, 0x9,+0 }, // 2298: f48GM107; Koto
    { 0x1E31117,0x2E31114, 0x10,0x6E, 0xC,+0 }, // 2299: f48GM115; Woodblock
    { 0x0E31111,0x0E31111, 0x80,0x00, 0xC,+0 }, // 2300: f48GM115; Woodblock
    { 0x017A821,0x0042571, 0x23,0x00, 0xA,+0 }, // 2301: f48GM116; Taiko Drum
    { 0x45FF811,0x0EFF310, 0x4F,0x00, 0xE,+0 }, // 2302: f48GM117; Melodic Tom
    { 0x15FF630,0x0EFF410, 0x12,0x00, 0xF,+0 }, // 2303: f48GM117; Melodic Tom
    { 0x00F4F2F,0x30F3F20, 0x00,0x00, 0xC,+0 }, // 2304: f48GM119; Reverse Cymbal
    { 0x03FF923,0x2FF9222, 0x23,0x0A, 0xE,+0 }, // 2305: f48GM120; Guitar FretNoise
    { 0x0BF9122,0x04FA123, 0x18,0x00, 0xE,+0 }, // 2306: f48GM120; Guitar FretNoise
    { 0x000F80F,0x3F93410, 0x00,0x05, 0xE,+0 }, // 2307: f48GM121; Breath Noise
    { 0x034A121,0x0166521, 0x17,0x00, 0xC,+0 }, // 2308: f48GM122; Seashore
    { 0x0FA6848,0x04AAA01, 0x00,0x3F, 0x5,+0 }, // 2309: f48GM123; Bird Tweet
    { 0x0FA6747,0x0FA464C, 0x00,0x00, 0x5,+0 }, // 2310: f48GM123; Bird Tweet
    { 0x2037F21,0x1065F61, 0x18,0x00, 0x0,+0 }, // 2311: f48GM124; Telephone
    { 0x10C2EF0,0x10C21E2, 0x00,0x00, 0x4,-36 }, // 2312: f48GM125; Helicopter
    { 0x70C2EF0,0x10C21E2, 0x00,0x00, 0x5,+0 }, // 2313: f48GM125; Helicopter
    { 0x039A321,0x03C7461, 0x8D,0x03, 0xA,+0 }, // 2314: f48GM126; Applause/Noise
    { 0x179A3A1,0x14C2321, 0x1C,0x00, 0xB,+0 }, // 2315: f48GM126; Applause/Noise
    { 0x01A7521,0x00F8F21, 0x97,0x00, 0xC,+0 }, // 2316: f48GM127; Gunshot
    { 0x0FFF920,0x0FFF620, 0xC0,0x00, 0x8,+0 }, // 2317: f48GP35; Ac Bass Drum
    { 0x277F810,0x0AFF611, 0x44,0x00, 0x8,+0 }, // 2318: f48GP36; Bass Drum 1
    { 0x01FF933,0x0FFF810, 0x80,0x00, 0x4,+0 }, // 2319: f48GP37; Side Stick
    { 0x2FFF500,0x0FFF700, 0x00,0x00, 0xC,+0 }, // 2320: f48GP38; Acoustic Snare
    { 0x0DFF712,0x0DFF811, 0x08,0x00, 0x2,+0 }, // 2321: f48GP39; Hand Clap
    { 0x0FFF210,0x0FFF510, 0x00,0x00, 0xC,+0 }, // 2322: f48GP40; Electric Snare
    { 0x1DFE920,0x0CEF400, 0x00,0x00, 0x4,+0 }, // 2323: f48GP41; f48GP43; High Floor Tom; Low Floor Tom
    { 0x2DFF50E,0x0AFF712, 0x00,0x00, 0xE,+0 }, // 2324: f48GP42; Closed High Hat
    { 0x03FF800,0x1FFF410, 0x03,0x00, 0x4,+0 }, // 2325: f48GP45; f48GP47; f48GP48; f48GP50; High Tom; High-Mid Tom; Low Tom; Low-Mid Tom
    { 0x2FFF012,0x3BF8608, 0x11,0x80, 0xE,+0 }, // 2326: f48GP46; Open High Hat
    { 0x0FFF20E,0x2DF9502, 0x00,0x00, 0xC,+0 }, // 2327: f48GP49; Crash Cymbal 1
    { 0x04FF82E,0x3EFF521, 0x07,0x0B, 0xE,+0 }, // 2328: f48GP51; Ride Cymbal 1
    { 0x2DDF014,0x0FF93F0, 0x00,0x00, 0xE,+0 }, // 2329: f48GP52; Chinese Cymbal
    { 0x3EFE40E,0x1EFF507, 0x0A,0x40, 0x6,+0 }, // 2330: f48GP53; Ride Bell
    { 0x0EFB402,0x0FF9705, 0x03,0x0A, 0xE,+0 }, // 2331: f48GP54; Tambourine
    { 0x01FF66E,0x3FF945E, 0x08,0x00, 0xE,+0 }, // 2332: f48GP55; Splash Cymbal
    { 0x200F6CE,0x3FFF21A, 0x04,0x00, 0xC,+0 }, // 2333: f48GP57; Crash Cymbal 2
    { 0x3FFF040,0x0FEF510, 0x00,0x00, 0xC,+0 }, // 2334: f48GP58; Vibraslap
    { 0x05EFD2E,0x3EFF527, 0x07,0x0C, 0xE,+0 }, // 2335: f48GP59; Ride Cymbal 2
    { 0x38CF803,0x0BCF60C, 0x80,0x08, 0xF,+0 }, // 2336: f48GP67; High Agogo
    { 0x38FF803,0x0BFF60C, 0x85,0x00, 0xF,+0 }, // 2337: f48GP68; Low Agogo
    { 0x04F760E,0x2CF7800, 0x40,0x08, 0xE,+0 }, // 2338: f48GP69; Cabasa
    { 0x04FC80E,0x26F9903, 0x40,0x00, 0xE,+0 }, // 2339: f48GP70; Maracas
    { 0x1DF75CE,0x2EF38E1, 0x00,0x00, 0xE,+0 }, // 2340: f48GP73; Short Guiro
    { 0x03FF162,0x0FF4B20, 0x00,0x00, 0x8,+0 }, // 2341: f48GP74; Long Guiro
    { 0x0F40006,0x0FBF715, 0x3F,0x00, 0x1,+0 }, // 2342: f48GP75; Claves
    { 0x0FF47E1,0x0FF47EA, 0x00,0x00, 0x0,+0 }, // 2343: f48GP78; Mute Cuica
    { 0x3FFE00A,0x0FFF51E, 0x40,0x0E, 0x8,+0 }, // 2344: f48GP80; Mute Triangle
    { 0x3FFE00A,0x0FFF21E, 0x7C,0x52, 0x8,+0 }, // 2345: f48GP81; Open Triangle
    { 0x04E7A0E,0x21E7B00, 0x81,0x00, 0xE,+0 }, // 2346: f48GP82; Shaker
    { 0x35FF925,0x0FFD524, 0x05,0x40, 0xE,+0 }, // 2347: f48GP84; Bell Tree
    { 0x08FFA01,0x0FFF802, 0x4F,0x00, 0x7,+0 }, // 2348: f48GP86; Mute Surdu
    { 0x0FFFC00,0x0FFF520, 0x00,0x00, 0x4,+0 }, // 2349: f48GP87; Open Surdu
    { 0x0F2B913,0x0119102, 0x0D,0x1A, 0xA,+0 }, // 2350: f49GM0; f49GM29; AcouGrandPiano; Overdrive Guitar
    { 0x74A9221,0x02A9122, 0x8F,0x00, 0xA,+0 }, // 2351: f49GM0; f49GM29; AcouGrandPiano; Overdrive Guitar
    { 0x60FF331,0x70FB135, 0x94,0xD5, 0xF,+0 }, // 2352: f49GM2; ElecGrandPiano
    { 0x302B133,0x305B131, 0x63,0x00, 0xE,+0 }, // 2353: f49GM2; ElecGrandPiano
    { 0x04F270C,0x0F8D104, 0x98,0x90, 0x8,+0 }, // 2354: f49GM3; Honky-tonkPiano
    { 0x0F8F502,0x0F8F402, 0x96,0x00, 0x9,+0 }, // 2355: f49GM3; Honky-tonkPiano
    { 0x759F201,0x600F701, 0x40,0x00, 0x0,+0 }, // 2356: f49GM111; f49GM7; Clavinet; Shanai
    { 0x6F0F301,0x7C9F601, 0x00,0x00, 0x0,+0 }, // 2357: f49GM111; f49GM7; Clavinet; Shanai
    { 0x60FFF15,0x66FB115, 0xC0,0x40, 0xE,+0 }, // 2358: f49GM32; f49GM8; Acoustic Bass; Celesta
    { 0x68FB111,0x6EFB111, 0x40,0x00, 0xE,+0 }, // 2359: f49GM32; f49GM8; Acoustic Bass; Celesta
    { 0x44FF920,0x2FF9122, 0x80,0x09, 0xE,+0 }, // 2360: f49GM24; f49GM9; Acoustic Guitar1; Glockenspiel
    { 0x7BF9121,0x64F9122, 0x99,0x00, 0xE,+0 }, // 2361: f49GM24; f49GM9; Acoustic Guitar1; Glockenspiel
    { 0x00AAFE1,0x00AAF62, 0x11,0x00, 0x9,+0 }, // 2362: f49GM18; Rock Organ
    { 0x022FA02,0x0F3F501, 0x4C,0x97, 0x8,+0 }, // 2363: f49GM27; Electric Guitar2
    { 0x1F3C504,0x0F7C511, 0x9D,0x00, 0x8,+0 }, // 2364: f49GM27; Electric Guitar2
    { 0x0AFC711,0x0F8F501, 0x8D,0x04, 0x8,+0 }, // 2365: f49GM28; Electric Guitar3
    { 0x098C301,0x0F8C302, 0x18,0x06, 0x9,+0 }, // 2366: f49GM28; Electric Guitar3
    { 0x40FF923,0x20F9122, 0x90,0x1B, 0xE,+0 }, // 2367: f49GM30; Distorton Guitar
    { 0x00F9121,0x00F9122, 0x9F,0x00, 0xE,+0 }, // 2368: f49GM30; Distorton Guitar
    { 0x60FFF15,0x61FB015, 0xC0,0x40, 0xE,+0 }, // 2369: f49GM31; Guitar Harmonics
    { 0x65FB111,0x63FB011, 0x40,0x00, 0xE,+0 }, // 2370: f49GM31; Guitar Harmonics
    { 0x60FFF35,0x60FB135, 0xC0,0x40, 0xE,+0 }, // 2371: f49GM33; Electric Bass 1
    { 0x6BFB131,0x60FB131, 0x40,0x00, 0xE,+0 }, // 2372: f49GM33; Electric Bass 1
    { 0x0C8F121,0x0C8F501, 0x13,0x29, 0x6,+0 }, // 2373: f49GM34; Electric Bass 2
    { 0x0C8F501,0x0C8F401, 0x14,0x00, 0x6,+0 }, // 2374: f49GM34; Electric Bass 2
    { 0x09AF381,0x0DFF521, 0x89,0x40, 0x6,+0 }, // 2375: f49GM37; Slap Bass 2
    { 0x0C8F121,0x0C8F701, 0x0F,0x25, 0xA,+0 }, // 2376: f49GM38; Synth Bass 1
    { 0x0C8F601,0x0C8F601, 0x12,0x00, 0xA,+0 }, // 2377: f49GM38; Synth Bass 1
    { 0x105F510,0x0C5F411, 0x41,0x00, 0x2,+0 }, // 2378: f49GM47; Timpany
    { 0x005F511,0x0C5F212, 0x01,0x1E, 0x3,+0 }, // 2379: f49GM47; Timpany
    { 0x012C1A1,0x0076F21, 0x93,0x00, 0xD,+0 }, // 2380: f49GM50; Synth Strings 1
    { 0x011DA65,0x068A663, 0x00,0x1B, 0xC,+0 }, // 2381: f49GM75; Pan Flute
    { 0x0588861,0x01A6561, 0x0A,0x00, 0xD,+0 }, // 2382: f49GM75; Pan Flute
    { 0x00FFF21,0x409CF61, 0x1D,0x05, 0xA,+0 }, // 2383: f49GM79; Ocarina
    { 0x70FFF20,0x30FFF61, 0x1A,0x14, 0x0,+0 }, // 2384: f49GM85; Lead 6 voice
    { 0x00FFF61,0x609CF61, 0x1A,0x07, 0x0,+0 }, // 2385: f49GM85; Lead 6 voice
    { 0x10D5317,0x00E3608, 0x1A,0x0D, 0x2,+0 }, // 2386: f49GM86; Lead 7 fifths
    { 0x03D41A1,0x01E6161, 0x9D,0x00, 0x3,+0 }, // 2387: f49GM86; f49GM88; Lead 7 fifths; Pad 1 new age
    { 0x0FC8561,0x4FD8463, 0x15,0x07, 0xA,+0 }, // 2388: f49GM87; Lead 8 brass
    { 0x0FD8966,0x6FC7761, 0x1F,0x00, 0xB,+0 }, // 2389: f49GM87; Lead 8 brass
    { 0x10A5317,0x0033608, 0x1A,0x0D, 0x2,+0 }, // 2390: f49GM88; Pad 1 new age
    { 0x0041121,0x3355261, 0x8C,0x00, 0x1,+0 }, // 2391: f49GM95; Pad 8 sweep
    { 0x0C6F521,0x096F461, 0x92,0x8A, 0xC,+0 }, // 2392: f49GM99; FX 4 atmosphere
    { 0x266F521,0x496F5A1, 0x90,0x80, 0xD,+0 }, // 2393: f49GM99; FX 4 atmosphere
    { 0x035D493,0x114EB11, 0x91,0x00, 0x8,+0 }, // 2394: f49GM107; Koto
    { 0x035D453,0x116EB13, 0x91,0x0D, 0x9,+0 }, // 2395: f49GM107; Koto
    { 0x56FF500,0x40FF300, 0x08,0x00, 0x1,+0 }, // 2396: f49GM112; Tinkle Bell
    { 0x65FF604,0x38FF580, 0x00,0x40, 0x0,+0 }, // 2397: f49GM112; Tinkle Bell
    { 0x66FF100,0x40FF300, 0x09,0x00, 0x0,+0 }, // 2398: f49GM113; Agogo Bells
    { 0x65FF601,0x73FF580, 0x1C,0x00, 0x0,+0 }, // 2399: f49GM113; Agogo Bells
    { 0x00F112F,0x30F1120, 0x00,0x00, 0xE,+0 }, // 2400: f49GM119; Reverse Cymbal
    { 0x00F1129,0x30F1120, 0x38,0x35, 0xF,+0 }, // 2401: f49GM119; Reverse Cymbal
    { 0x024F806,0x7845603, 0x00,0x04, 0xE,+0 }, // 2402: f49GM120; Guitar FretNoise
    { 0x624D803,0x784F604, 0x0B,0x00, 0xF,+0 }, // 2403: f49GM120; Guitar FretNoise
    { 0x624F802,0x7845604, 0x00,0x04, 0xA,+0 }, // 2404: f49GM121; Breath Noise
    { 0x624D800,0x784F603, 0x0B,0x00, 0xB,+0 }, // 2405: f49GM121; Breath Noise
    { 0x46FF220,0x07FF400, 0x14,0x00, 0xF,+1 }, // 2406: f49GM124; Telephone
    { 0x01FF501,0x51FF487, 0x00,0xC0, 0xF,+0 }, // 2407: f49GM124; Telephone
    { 0x059F200,0x700F701, 0x00,0x00, 0xE,+0 }, // 2408: f49GM126; Applause/Noise
    { 0x0F0F301,0x6C9F401, 0x00,0x00, 0xE,+0 }, // 2409: f49GM126; Applause/Noise
    { 0x0F7F810,0x006F211, 0x40,0x00, 0x8,+0 }, // 2410: f49GP35; f49GP36; Ac Bass Drum; Bass Drum 1
    { 0x002F010,0x006FE00, 0x00,0x00, 0xC,+0 }, // 2411: f49GP40; Electric Snare
    { 0x207F70E,0x008FF12, 0x00,0x00, 0xE,+0 }, // 2412: f49GP42; Closed High Hat
    { 0x092FF83,0x003F015, 0x00,0x00, 0xE,+0 }, // 2413: f50GM40; f50GM78; Violin; Whistle
    { 0x0F4C306,0x0E4C203, 0xB5,0x76, 0x4,+0 }, // 2414: f53GM0; AcouGrandPiano
    { 0x0D4C101,0x0E5B111, 0x53,0x02, 0x4,+0 }, // 2415: f53GM0; AcouGrandPiano
    { 0x0F3C301,0x0F3C307, 0xA1,0x70, 0xC,+12 }, // 2416: f53GM1; BrightAcouGrand
    { 0x034B000,0x0F5A111, 0xCC,0x00, 0xC,+0 }, // 2417: f53GM1; BrightAcouGrand
    { 0x034FB31,0x0F7C131, 0x93,0x00, 0xC,+0 }, // 2418: f53GM2; ElecGrandPiano
    { 0x0DFB811,0x0F7F121, 0x97,0x8B, 0xD,+0 }, // 2419: f53GM2; ElecGrandPiano
    { 0x0E4A115,0x0E4A115, 0x6A,0x67, 0xE,+0 }, // 2420: f53GM3; Honky-tonkPiano
    { 0x0E4A111,0x0E5A111, 0x55,0x03, 0xE,+0 }, // 2421: f53GM3; Honky-tonkPiano
    { 0x0E7C21A,0x0E7C201, 0x33,0x85, 0x0,+0 }, // 2422: f53GM4; Rhodes Piano
    { 0x0F4B111,0x0E4B111, 0x1D,0x83, 0x1,+0 }, // 2423: f53GM4; Rhodes Piano
    { 0x0E7C21C,0x0E6C201, 0xBD,0x8B, 0xE,+0 }, // 2424: f53GM5; f54GM99; Chorused Piano; FX 4 atmosphere
    { 0x0E4B111,0x0E5B111, 0x52,0x85, 0xF,+0 }, // 2425: f53GM5; f54GM99; Chorused Piano; FX 4 atmosphere
    { 0x050F210,0x0F0E12A, 0xA1,0x64, 0xE,+12 }, // 2426: f53GM6; Harpsichord
    { 0x020BD20,0x0E7C112, 0x19,0x03, 0xE,+0 }, // 2427: f53GM6; Harpsichord
    { 0x12A91B1,0x00AF021, 0x80,0xA1, 0x7,+12 }, // 2428: f53GM7; Clavinet
    { 0x038D620,0x0B7F8A6, 0x03,0x05, 0x7,+0 }, // 2429: f53GM7; Clavinet
    { 0x017F820,0x0057F31, 0x94,0x08, 0xC,+12 }, // 2430: f53GM8; Celesta
    { 0x029F623,0x00A8F22, 0x1E,0x0B, 0xD,+0 }, // 2431: f53GM8; Celesta
    { 0x00AB028,0x00AB0A1, 0x5A,0x21, 0x1,+0 }, // 2432: f53GM9; Glockenspiel
    { 0x00A8024,0x00AB021, 0xC0,0x09, 0x1,+0 }, // 2433: f53GM9; Glockenspiel
    { 0x00AF0A2,0x00AF024, 0x06,0xA1, 0x5,+0 }, // 2434: f53GM10; Music box
    { 0x00AF0A4,0x00AF021, 0x0A,0x06, 0x5,+0 }, // 2435: f53GM10; Music box
    { 0x00FFF27,0x00FFF21, 0x29,0x07, 0x0,+0 }, // 2436: f53GM11; Vibraphone
    { 0x00FFF21,0x00FFF22, 0x18,0x06, 0x1,+0 }, // 2437: f53GM11; Vibraphone
    { 0x00AFF61,0x00AFF22, 0x0E,0xA1, 0x7,+0 }, // 2438: f53GM12; Marimba
    { 0x00AFF64,0x00AFF21, 0x0A,0x0B, 0x7,+0 }, // 2439: f53GM12; Marimba
    { 0x00FFF20,0x00FFFA1, 0x22,0x88, 0xC,+12 }, // 2440: f53GM13; Xylophone
    { 0x00FFF22,0x00FFFA1, 0x56,0x84, 0xD,+0 }, // 2441: f53GM13; Xylophone
    { 0x0F6EA09,0x0F4F518, 0x0F,0x8C, 0x0,+0 }, // 2442: f53GM14; Tubular Bells
    { 0x00FEFA2,0x00B8F21, 0x3E,0x07, 0x1,+0 }, // 2443: f53GM14; Tubular Bells
    { 0x0186223,0x02A6221, 0x1C,0x87, 0xE,+0 }, // 2444: f53GM15; Dulcimer
    { 0x1186223,0x02A62A2, 0x19,0x82, 0xF,+0 }, // 2445: f53GM15; Dulcimer
    { 0x001F201,0x0F1F101, 0x21,0x1D, 0xA,+0 }, // 2446: f53GM16; Hammond Organ
    { 0x0E3F301,0x0E6F211, 0x4B,0x00, 0xA,+0 }, // 2447: f53GM16; Hammond Organ
    { 0x030FE10,0x0F0E13A, 0x9F,0x65, 0xE,+12 }, // 2448: f53GM17; Percussive Organ
    { 0x020BD20,0x0E7C112, 0x8D,0x07, 0xE,+0 }, // 2449: f53GM17; Percussive Organ
    { 0x025F5E2,0x005EF24, 0x1E,0x9F, 0xE,-12 }, // 2450: f53GM18; Rock Organ
    { 0x004EF26,0x006CF24, 0x9E,0x06, 0xE,+0 }, // 2451: f53GM18; Rock Organ
    { 0x043D227,0x0E4E215, 0x9A,0x03, 0x8,-12 }, // 2452: f53GM19; Church Organ
    { 0x023A7B7,0x0E4C215, 0x19,0x08, 0x9,+0 }, // 2453: f53GM19; Church Organ
    { 0x043D223,0x0E4E212, 0x98,0x03, 0x8,+0 }, // 2454: f53GM20; Reed Organ
    { 0x023A7B3,0x0E4C212, 0x19,0x08, 0x9,+0 }, // 2455: f53GM20; Reed Organ
    { 0x0E6CE22,0x0E6F421, 0x25,0x03, 0x0,+0 }, // 2456: f53GM21; Accordion
    { 0x0E6F727,0x0E5F521, 0x32,0x09, 0x1,+0 }, // 2457: f53GM21; Accordion
    { 0x006F504,0x041F001, 0x3F,0x05, 0x0,+0 }, // 2458: f53GM22; f53GM23; Harmonica; Tango Accordion
    { 0x035D208,0x005F120, 0x00,0x06, 0x0,+0 }, // 2459: f53GM22; Harmonica
    { 0x034D201,0x003F120, 0x00,0x06, 0x0,+0 }, // 2460: f53GM23; Tango Accordion
    { 0x0276621,0x0486621, 0x1C,0x00, 0xE,+0 }, // 2461: f53GM24; Acoustic Guitar1
    { 0x00A6621,0x0486621, 0x94,0x00, 0xF,+0 }, // 2462: f53GM24; Acoustic Guitar1
    { 0x0E44100,0x0046620, 0x91,0x08, 0xC,+12 }, // 2463: f53GM25; Acoustic Guitar2
    { 0x0E65120,0x0066620, 0x8E,0x08, 0xD,+0 }, // 2464: f53GM25; Acoustic Guitar2
    { 0x0257521,0x00AAF21, 0x1A,0x08, 0xE,+0 }, // 2465: f53GM26; Electric Guitar1
    { 0x0257521,0x00AAF21, 0x1A,0x0C, 0xF,+0 }, // 2466: f53GM26; Electric Guitar1
    { 0x015A221,0x00AAF21, 0x12,0x02, 0xC,+0 }, // 2467: f53GM27; Electric Guitar2
    { 0x055F2A1,0x00AAF21, 0x28,0x05, 0xD,+0 }, // 2468: f53GM27; Electric Guitar2
    { 0x0CFF416,0x0E6F205, 0x23,0x69, 0xC,+12 }, // 2469: f53GM28; Electric Guitar3
    { 0x0D5F200,0x0ECE301, 0x15,0x00, 0xC,+0 }, // 2470: f53GM28; Electric Guitar3
    { 0x058F620,0x05AF520, 0x98,0x19, 0xE,+12 }, // 2471: f53GM29; Overdrive Guitar
    { 0x009FF21,0x00CFF20, 0x24,0x00, 0xE,+0 }, // 2472: f53GM29; Overdrive Guitar
    { 0x006F801,0x0D5D500, 0x17,0x17, 0x8,+12 }, // 2473: f53GM30; Distorton Guitar
    { 0x4E6F511,0x0E8F500, 0x14,0x00, 0x8,+0 }, // 2474: f53GM30; Distorton Guitar
    { 0x045FB01,0x050FF12, 0x10,0x0C, 0x0,+12 }, // 2475: f53GM31; Guitar Harmonics
    { 0x034FF00,0x027F300, 0x16,0x00, 0x0,+0 }, // 2476: f53GM31; Guitar Harmonics
    { 0x0EAF50C,0x0E6F21F, 0x21,0x21, 0xE,+0 }, // 2477: f53GM32; Acoustic Bass
    { 0x0F6F401,0x0E7F113, 0x15,0x03, 0xE,+0 }, // 2478: f53GM32; Acoustic Bass
    { 0x0E6F407,0x0F6A114, 0x9B,0x1D, 0xE,+0 }, // 2479: f53GM33; Electric Bass 1
    { 0x00FFF21,0x0E6F112, 0x12,0x04, 0xE,+0 }, // 2480: f53GM33; Electric Bass 1
    { 0x062F227,0x062F231, 0x26,0x18, 0xC,+0 }, // 2481: f53GM34; Electric Bass 2
    { 0x066F521,0x0E4F116, 0x0E,0x03, 0xC,+0 }, // 2482: f53GM34; Electric Bass 2
    { 0x015A221,0x0DAC401, 0x13,0x14, 0xC,+0 }, // 2483: f53GM35; Fretless Bass
    { 0x055F221,0x0DAA401, 0x2A,0x00, 0xC,+0 }, // 2484: f53GM35; Fretless Bass
    { 0x00C0300,0x024FA20, 0x30,0x03, 0x6,+12 }, // 2485: f53GM36; Slap Bass 1
    { 0x024F820,0x056F510, 0x12,0x00, 0x6,+0 }, // 2486: f53GM36; Slap Bass 1
    { 0x09CF901,0x0F98701, 0x00,0x03, 0x6,+0 }, // 2487: f53GM37; Slap Bass 2
    { 0x0ACF904,0x0F98701, 0x00,0x00, 0x7,+0 }, // 2488: f53GM37; Slap Bass 2
    { 0x025F261,0x015F2A5, 0x22,0x5E, 0xE,+0 }, // 2489: f53GM38; Synth Bass 1
    { 0x015F223,0x0C6E111, 0x5B,0x02, 0xE,+0 }, // 2490: f53GM38; Synth Bass 1
    { 0x006FF22,0x00B9F22, 0x1C,0x08, 0xE,+0 }, // 2491: f53GM39; Synth Bass 2
    { 0x005FA21,0x00B9F21, 0x19,0x07, 0xF,+0 }, // 2492: f53GM39; Synth Bass 2
    { 0x0F6D133,0x0F7F221, 0x9A,0x03, 0xC,+0 }, // 2493: f53GM40; Violin
    { 0x0E4F22F,0x0F7F224, 0x28,0x8A, 0xD,+0 }, // 2494: f53GM40; Violin
    { 0x03FF43A,0x04FF231, 0x64,0x5A, 0xE,+0 }, // 2495: f53GM41; Viola
    { 0x024F211,0x085F311, 0x25,0x08, 0xE,+0 }, // 2496: f53GM41; Viola
    { 0x026F211,0x04FF43A, 0x23,0x5F, 0xE,+0 }, // 2497: f53GM42; Cello
    { 0x04FF231,0x0D6F211, 0x63,0x07, 0xE,+0 }, // 2498: f53GM42; Cello
    { 0x03AA021,0x097A123, 0x23,0x21, 0xE,+12 }, // 2499: f53GM43; Contrabass
    { 0x0F2A310,0x0F5A020, 0x12,0x05, 0xE,+0 }, // 2500: f53GM43; Contrabass
    { 0x030F70C,0x0A8F101, 0x23,0x26, 0xA,+0 }, // 2501: f53GM44; Tremulo Strings
    { 0x0C6F201,0x043F212, 0x13,0x00, 0xA,+0 }, // 2502: f53GM44; Tremulo Strings
    { 0x054D41F,0x0F5C411, 0x65,0x42, 0xC,+0 }, // 2503: f53GM45; Pizzicato String
    { 0x0F4B113,0x0E5A111, 0x50,0x05, 0xD,+0 }, // 2504: f53GM45; Pizzicato String
    { 0x0AFF505,0x03DFD2C, 0x3F,0x13, 0xA,+0 }, // 2505: f53GM46; Orchestral Harp
    { 0x0B0F607,0x074F411, 0x0F,0x08, 0xA,+0 }, // 2506: f53GM46; Orchestral Harp
    { 0x022E832,0x0F5B210, 0x08,0x12, 0x2,+12 }, // 2507: f53GM47; Timpany
    { 0x021F730,0x0F5B214, 0x08,0x0D, 0x3,+0 }, // 2508: f53GM47; Timpany
    { 0x025F5E2,0x005EF24, 0x20,0x9F, 0xE,-12 }, // 2509: f53GM48; f53GM51; String Ensemble1; SynthStrings 2
    { 0x004EF26,0x0065F24, 0x9E,0x06, 0xE,+0 }, // 2510: f53GM48; f53GM51; String Ensemble1; SynthStrings 2
    { 0x004EFE2,0x005EF24, 0x24,0x21, 0xE,-12 }, // 2511: f53GM49; String Ensemble2
    { 0x004EF26,0x0065F24, 0x9F,0x07, 0xE,+0 }, // 2512: f53GM49; String Ensemble2
    { 0x002EFE2,0x003EF24, 0xAA,0xA1, 0xE,-12 }, // 2513: f53GM50; Synth Strings 1
    { 0x003EF26,0x0065F24, 0xA4,0x03, 0xE,+0 }, // 2514: f53GM50; Synth Strings 1
    { 0x016D122,0x0055572, 0x9A,0x06, 0xE,-12 }, // 2515: f53GM52; Choir Aahs
    { 0x0F6C102,0x2055571, 0xD9,0x0D, 0xF,+0 }, // 2516: f53GM52; Choir Aahs
    { 0x012F322,0x0054F22, 0x1D,0x04, 0xE,+0 }, // 2517: f53GM53; Voice Oohs
    { 0x013F321,0x0054F22, 0x91,0x80, 0xF,+0 }, // 2518: f53GM53; Voice Oohs
    { 0x015F322,0x0065F22, 0x1D,0x05, 0xE,+0 }, // 2519: f53GM54; Synth Voice
    { 0x015F321,0x0075F23, 0x91,0x80, 0xF,+0 }, // 2520: f53GM54; Synth Voice
    { 0x295F520,0x353F411, 0x90,0x00, 0xC,+12 }, // 2521: f53GM55; Orchestra Hit
    { 0x295F520,0x353F411, 0x90,0x09, 0xC,+12 }, // 2522: f53GM56; Trumpet
    { 0x0FAF52F,0x0FAF423, 0xB2,0x64, 0xE,+0 }, // 2523: f53GM57; Trombone
    { 0x0FAE323,0x0FAF321, 0x66,0x03, 0xE,+0 }, // 2524: f53GM57; Trombone
    { 0x036D122,0x0055572, 0x9A,0x00, 0xE,-12 }, // 2525: f53GM58; Tuba
    { 0x4F6C102,0x2055574, 0xD9,0x07, 0xF,+0 }, // 2526: f53GM58; Tuba
    { 0x0D6F328,0x0F9F423, 0xA2,0x5F, 0xC,+0 }, // 2527: f53GM59; Muted Trumpet
    { 0x0F8E223,0x0E8F301, 0xA6,0x03, 0xC,+0 }, // 2528: f53GM59; Muted Trumpet
    { 0x01AD1A1,0x00A9F22, 0x2C,0x8F, 0xE,+0 }, // 2529: f53GM60; French Horn
    { 0x00A9F22,0x00A9F22, 0x0F,0x08, 0xE,+0 }, // 2530: f53GM60; French Horn
    { 0x020FE70,0x0E9C212, 0x13,0x80, 0xA,+12 }, // 2531: f53GM61; Brass Section
    { 0x07FBC20,0x0E9C212, 0x11,0x05, 0xB,+0 }, // 2532: f53GM61; Brass Section
    { 0x020FE10,0x0E7C212, 0x12,0x00, 0xC,+12 }, // 2533: f53GM62; Synth Brass 1
    { 0x053BD00,0x0E7C212, 0x15,0x07, 0xD,+0 }, // 2534: f53GM62; Synth Brass 1
    { 0x0E54151,0x0E8F652, 0xA9,0x63, 0xE,+0 }, // 2535: f53GM64; f54GM37; Slap Bass 2; Soprano Sax
    { 0x0E8D151,0x0E6C251, 0x9B,0x80, 0xE,+0 }, // 2536: f53GM64; f54GM37; Slap Bass 2; Soprano Sax
    { 0x0C8F621,0x0F8F821, 0x1D,0x23, 0xE,+12 }, // 2537: f53GM65; Alto Sax
    { 0x0F8F420,0x0F8F320, 0x20,0x00, 0xE,+0 }, // 2538: f53GM65; Alto Sax
    { 0x058F520,0x059F520, 0x9B,0x19, 0xE,+12 }, // 2539: f53GM66; Tenor Sax
    { 0x089F320,0x00CFF20, 0x19,0x07, 0xE,+0 }, // 2540: f53GM66; Tenor Sax
    { 0x061F800,0x0EAF582, 0x2B,0x15, 0xC,+12 }, // 2541: f53GM67; Baritone Sax
    { 0x0FFF420,0x097F400, 0x1B,0x00, 0xC,+0 }, // 2542: f53GM67; Baritone Sax
    { 0x0E54711,0x0E68511, 0x29,0x1E, 0xE,+0 }, // 2543: f53GM68; Oboe
    { 0x0E8F512,0x0E6C251, 0x5E,0x40, 0xE,+0 }, // 2544: f53GM68; Oboe
    { 0x010F101,0x0C2F101, 0x35,0x17, 0xA,+0 }, // 2545: f53GM69; English Horn
    { 0x0C4F307,0x0E3F212, 0x12,0x00, 0xA,+0 }, // 2546: f53GM69; English Horn
    { 0x0DFF63C,0x0DFF521, 0xA7,0x18, 0xE,+12 }, // 2547: f53GM70; Bassoon
    { 0x0D7F220,0x0E8F320, 0x1A,0x00, 0xE,+0 }, // 2548: f53GM70; Bassoon
    { 0x0A0F400,0x0A7F101, 0x05,0x26, 0xA,+12 }, // 2549: f53GM71; Clarinet
    { 0x0C5F201,0x043F212, 0x12,0x00, 0xA,+0 }, // 2550: f53GM71; Clarinet
    { 0x019AA2F,0x0CFF9A2, 0x00,0x1F, 0xE,+0 }, // 2551: f53GM72; Piccolo
    { 0x015FAA1,0x00B7F21, 0x9F,0x06, 0xE,+0 }, // 2552: f53GM72; Piccolo
    { 0x01171B1,0x1E54141, 0x8B,0x40, 0x6,+0 }, // 2553: f53GM74; Recorder
    { 0x0AE7101,0x0EE8101, 0x1E,0x00, 0xE,+0 }, // 2554: f53GM75; Pan Flute
    { 0x0AE7101,0x0EE8101, 0x20,0x00, 0xE,+0 }, // 2555: f53GM76; Bottle Blow
    { 0x016D322,0x02764B2, 0x9A,0x04, 0xE,-12 }, // 2556: f53GM78; Whistle
    { 0x006C524,0x02764B2, 0x61,0x09, 0xF,+0 }, // 2557: f53GM78; Whistle
    { 0x0066231,0x0E7A241, 0x1E,0x80, 0xE,+0 }, // 2558: f53GM79; Ocarina
    { 0x0AE7101,0x0EE8101, 0x1C,0x00, 0xE,+0 }, // 2559: f53GM80; Lead 1 squareea
    { 0x2129A13,0x0119B91, 0x97,0x80, 0xE,+0 }, // 2560: f53GM81; Lead 2 sawtooth
    { 0x0056F22,0x0094F31, 0x56,0x0A, 0x8,+0 }, // 2561: f53GM83; Lead 4 chiff
    { 0x0056F22,0x0094FB1, 0x59,0x0C, 0x9,+0 }, // 2562: f53GM83; Lead 4 chiff
    { 0x1298920,0x1268532, 0x1F,0x5F, 0x0,+12 }, // 2563: f53GM85; Lead 6 voice
    { 0x0159AA0,0x01A8D22, 0x4C,0x03, 0x0,+0 }, // 2564: f53GM85; Lead 6 voice
    { 0x007CF20,0x0E97102, 0x5B,0x00, 0xE,+0 }, // 2565: f53GM86; Lead 7 fifths
    { 0x0014131,0x03B9261, 0x99,0x80, 0xE,+0 }, // 2566: f53GM87; Lead 8 brass
    { 0x0014131,0x03B9261, 0x1C,0x80, 0xE,+0 }, // 2567: f53GM88; Pad 1 new age
    { 0x0475421,0x0097F21, 0x1D,0x07, 0xE,+0 }, // 2568: f53GM90; Pad 3 polysynth
    { 0x0476421,0x0087F61, 0x19,0x0B, 0xF,+0 }, // 2569: f53GM90; Pad 3 polysynth
    { 0x0176421,0x0098F21, 0x98,0x07, 0xE,+0 }, // 2570: f53GM91; Pad 4 choir
    { 0x0176421,0x0087F61, 0x17,0x0F, 0xF,+0 }, // 2571: f53GM91; Pad 4 choir
    { 0x0296321,0x00A7F21, 0x22,0x03, 0xE,+0 }, // 2572: f53GM92; Pad 5 bowedpad
    { 0x0186521,0x00A7F61, 0x1B,0x0D, 0xF,+0 }, // 2573: f53GM92; Pad 5 bowedpad
    { 0x0156220,0x0E67141, 0x9A,0x00, 0xE,+12 }, // 2574: f53GM94; Pad 7 halo
    { 0x02651B1,0x0E65151, 0xDB,0x87, 0xF,+0 }, // 2575: f53GM94; Pad 7 halo
    { 0x02365A3,0x0059F21, 0x1C,0x1C, 0xE,+0 }, // 2576: f53GM95; Pad 8 sweep
    { 0x003DFA1,0x00BDF21, 0x1A,0x07, 0xE,+0 }, // 2577: f53GM95; Pad 8 sweep
    { 0x0014131,0x03B9261, 0x20,0x80, 0xE,+0 }, // 2578: f53GM96; FX 1 rain
    { 0x04AF823,0x0C5D283, 0xB5,0x52, 0x8,+12 }, // 2579: f53GM97; FX 2 soundtrack
    { 0x0E6F414,0x0D5F280, 0x99,0x00, 0x9,+0 }, // 2580: f53GM97; FX 2 soundtrack
    { 0x0FAF40C,0x0F4C212, 0x37,0x2B, 0x0,+0 }, // 2581: f53GM98; FX 3 crystal
    { 0x053F685,0x0E4F191, 0x64,0x00, 0x0,+0 }, // 2582: f53GM98; FX 3 crystal
    { 0x006F600,0x0E9F51F, 0x35,0x25, 0x0,+12 }, // 2583: f53GM99; FX 4 atmosphere
    { 0x000F023,0x0E5F280, 0x5E,0x00, 0x0,+0 }, // 2584: f53GM99; FX 4 atmosphere
    { 0x0F5F50C,0x0F5F2A1, 0xA9,0x05, 0xE,+0 }, // 2585: f53GM100; FX 5 brightness
    { 0x0F6F307,0x0F6F281, 0x31,0x04, 0xF,+0 }, // 2586: f53GM100; FX 5 brightness
    { 0x0E5F14F,0x0E5C301, 0x69,0x06, 0x8,+0 }, // 2587: f53GM101; FX 6 goblins
    { 0x052F605,0x0D5F281, 0x2D,0x03, 0x9,+0 }, // 2588: f53GM101; FX 6 goblins
    { 0x0E6F482,0x03AFE00, 0x0F,0x26, 0x1,+12 }, // 2589: f53GM102; FX 7 echoes
    { 0x0F6F380,0x0F5F787, 0x03,0x10, 0x1,+0 }, // 2590: f53GM102; FX 7 echoes
    { 0x0F5FD2C,0x0F5F427, 0x8E,0x20, 0x0,+0 }, // 2591: f53GM103; FX 8 sci-fi
    { 0x0F4F827,0x0F5F421, 0x20,0x00, 0x0,+0 }, // 2592: f53GM103; FX 8 sci-fi
    { 0x097CB05,0x0D5E801, 0x9F,0x00, 0xA,+0 }, // 2593: f53GM104; Sitar
    { 0x035F705,0x0E6E401, 0x28,0x05, 0xB,+0 }, // 2594: f53GM104; Sitar
    { 0x0095FE1,0x0076FE1, 0x58,0x03, 0x0,+0 }, // 2595: f53GM105; Banjo
    { 0x054890A,0x063A726, 0x6C,0x63, 0xA,+0 }, // 2596: f53GM107; Koto
    { 0x0094F21,0x0083F61, 0xCE,0x02, 0xA,+0 }, // 2597: f53GM107; Koto
    { 0x00F7F04,0x0CFF5EA, 0x30,0xA9, 0x8,+0 }, // 2598: f53GM108; Kalimba
    { 0x00F5F21,0x00AAF61, 0x1C,0x06, 0x8,+0 }, // 2599: f53GM108; Kalimba
    { 0x0549963,0x06AA768, 0x98,0xA9, 0xE,+0 }, // 2600: f53GM109; Bagpipe
    { 0x0095F61,0x0097F61, 0xD1,0x03, 0xE,+0 }, // 2601: f53GM109; Bagpipe
    { 0x0549963,0x06AA768, 0xD4,0x5E, 0xE,+0 }, // 2602: f53GM110; Fiddle
    { 0x0095F61,0x0097F61, 0xC9,0x06, 0xE,+0 }, // 2603: f53GM110; Fiddle
    { 0x0B643A1,0x0B6F6A3, 0x2A,0xB0, 0xE,+0 }, // 2604: f53GM111; Shanai
    { 0x0067FA1,0x0066F61, 0x2C,0x02, 0xE,+0 }, // 2605: f53GM111; Shanai
    { 0x053F101,0x0B5F700, 0x73,0x00, 0x6,+0 }, // 2606: f53GM112; Tinkle Bell
    { 0x021A121,0x116C221, 0x92,0x40, 0x6,+0 }, // 2607: f53GM113; Agogo Bells
    { 0x024A80F,0x005DF02, 0xB8,0x03, 0x0,-12 }, // 2608: f53GM116; Taiko Drum
    { 0x035A70A,0x005DF02, 0xA2,0x03, 0x1,+0 }, // 2609: f53GM116; Taiko Drum
    { 0x01379C0,0x07372D2, 0x4F,0x00, 0x6,-12 }, // 2610: f53GM119; Reverse Cymbal
    { 0x013FA43,0x095F342, 0xD6,0x80, 0xA,-24 }, // 2611: f53GM120; Guitar FretNoise
    { 0x020D933,0x0E4B211, 0x08,0x08, 0x6,+0 }, // 2612: f53GM121; Breath Noise
    { 0x02278B0,0x0E4B214, 0x06,0x0D, 0x7,+0 }, // 2613: f53GM121; Breath Noise
    { 0x10475A0,0x0057221, 0x12,0x40, 0x6,+0 }, // 2614: f53GM123; Bird Tweet
    { 0x0F1F007,0x0349800, 0x00,0x00, 0xE,+0 }, // 2615: f53GM124; Telephone
    { 0x1137521,0x0B47182, 0x92,0x40, 0xA,+0 }, // 2616: f53GM126; Applause/Noise
    { 0x6B5F100,0x6B8F100, 0xD5,0x51, 0xB,+0 }, // 2617: f53GM126; Applause/Noise
    { 0x0F0F601,0x0E2F01C, 0x3F,0x1C, 0x8,+0 }, // 2618: f53GM127; Gunshot
    { 0x003F103,0x093F0A0, 0x00,0x00, 0x8,+0 }, // 2619: f53GM127; Gunshot
    { 0x163F401,0x174F111, 0x12,0x00, 0xA,+0 }, // 2620: f54GM81; Lead 2 sawtooth
    { 0x201F130,0x083F001, 0x44,0x83, 0xA,+0 }, // 2621: f54GM87; f54GM90; Lead 8 brass; Pad 3 polysynth
    { 0x002F002,0x004D001, 0xC0,0x00, 0x4,+0 }, // 2622: b41M0; china2.i
    { 0x154FF0A,0x0F5F002, 0x04,0x00, 0x0,+0 }, // 2623: b41M7; china1.i
    { 0x100FF22,0x10BF020, 0x92,0x00, 0x4,+0 }, // 2624: b41M9; car2.ins
    { 0x101F901,0x0F5F001, 0x34,0x00, 0x4,+0 }, // 2625: b41M28; bassharp
    { 0x01774E1,0x01765E2, 0x83,0x00, 0x7,+0 }, // 2626: b41M33; flute3.i
    { 0x154F103,0x054F10A, 0x00,0x00, 0x0,+0 }, // 2627: b41M126; b41M127; b41M34; sitar2.i
    { 0x000EA36,0x024DF1A, 0x8B,0x00, 0x8,+0 }, // 2628: b41M36; banjo3.i
    { 0x0115172,0x11552A2, 0x89,0x00, 0xA,+0 }, // 2629: b41M48; b41M50; strings1
    { 0x025DC03,0x009F031, 0x90,0x00, 0x8,+0 }, // 2630: b41M67; cstacc19
    { 0x0F16000,0x0F87001, 0x1D,0x00, 0xE,+0 }, // 2631: b41M88; tuba2.in
    { 0x1009F71,0x1069F22, 0x45,0x00, 0x2,+0 }, // 2632: b41M89; harmonc2
    { 0x204D983,0x004D081, 0x17,0x00, 0xE,+0 }, // 2633: b41M98; matilda.
    { 0x035F803,0x004FF01, 0x12,0x00, 0x8,+0 }, // 2634: b41M114; italy.in
    { 0x2129FD6,0x0F290D2, 0x17,0x00, 0x2,+0 }, // 2635: b41M123; entbell.
    { 0x0F00000,0x0F00000, 0x3F,0x3F, 0x0,+0 }, // 2636: b41P0; b41P1; b41P10; b41P100; b41P101; b41P102; b41P103; b41P104; b41P105; b41P106; b41P107; b41P108; b41P109; b41P11; b41P110; b41P111; b41P112; b41P113; b41P114; b41P115; b41P116; b41P117; b41P118; b41P119; b41P12; b41P120; b41P121; b41P122; b41P123; b41P124; b41P125; b41P126; b41P127; b41P13; b41P14; b41P15; b41P16; b41P17; b41P18; b41P19; b41P2; b41P20; b41P21; b41P22; b41P23; b41P24; b41P25; b41P26; b41P27; b41P28; b41P29; b41P3; b41P30; b41P31; b41P32; b41P33; b41P34; b41P4; b41P5; b41P51; b41P54; b41P55; b41P58; b41P59; b41P6; b41P7; b41P74; b41P77; b41P78; b41P79; b41P8; b41P80; b41P81; b41P82; b41P83; b41P84; b41P85; b41P86; b41P87; b41P88; b41P89; b41P9; b41P90; b41P91; b41P92; b41P93; b41P94; b41P95; b41P96; b41P97; b41P98; b41P99; blank.in
    { 0x26EF800,0x03FF600, 0x08,0x02, 0x0,+0 }, // 2637: b41P71; undersn.
    { 0x0FFB000,0x02F8607, 0x00,0x00, 0x0,+0 }, // 2638: b41P76; heart.in
    { 0x0977801,0x3988802, 0x00,0x00, 0x8,+0 }, // 2639: b42P29; b42P30; scratch
    { 0x06CF800,0x04AE80E, 0x00,0x80, 0x0,+0 }, // 2640: b42P32; hiq
    { 0x144F406,0x034F201, 0x03,0x1B, 0x1,+0 }, // 2641: b42M9; b42P34; GLOCK; glock
    { 0x0FDFA01,0x047F601, 0x07,0x00, 0x4,+0 }, // 2642: b42P36; Kick
    { 0x01FFA06,0x0F5F511, 0x0A,0x00, 0xD,+0 }, // 2643: b42P41; b42P43; b42P45; b42P47; b42P48; b42P50; Toms
    { 0x0FEF22C,0x3D8B802, 0x00,0x06, 0x6,+0 }, // 2644: b42P42; b42P44; clsdht47
    { 0x0F6822E,0x3F87404, 0x00,0x10, 0x4,+0 }, // 2645: b42P46; Openhat
    { 0x2009F2C,0x3D4C50E, 0x00,0x05, 0xE,+0 }, // 2646: b42P49; b42P52; b42P55; b42P57; Crash
    { 0x0009429,0x344F904, 0x10,0x04, 0xE,+0 }, // 2647: b42P51; b42P53; b42P59; Ride; ride
    { 0x0F1F52E,0x3F78706, 0x09,0x03, 0x0,+0 }, // 2648: b42P54; Tamb
    { 0x1A1F737,0x028F603, 0x14,0x00, 0x8,+0 }, // 2649: b42P56; Cowbell
    { 0x1FAFB21,0x0F7A802, 0x03,0x00, 0x0,+0 }, // 2650: b42P61; conga
    { 0x2FAF924,0x0F6A603, 0x18,0x00, 0xE,+0 }, // 2651: b42P63; b42P64; loconga
    { 0x2F5F505,0x236F603, 0x14,0x00, 0x6,+0 }, // 2652: b42P65; b42P66; timbale
    { 0x131F91C,0x1E89615, 0x0C,0x00, 0xE,+0 }, // 2653: b42M113; b42P67; b42P68; AGOGO; agogo
    { 0x001FF0E,0x377790E, 0x00,0x02, 0xE,+0 }, // 2654: b42P69; b42P70; b42P82; shaker
    { 0x107AF20,0x22BA50E, 0x15,0x00, 0x4,+0 }, // 2655: b42P71; hiwhist
    { 0x107BF20,0x23B930E, 0x18,0x00, 0x0,+0 }, // 2656: b42P72; lowhist
    { 0x0F7F020,0x33B8908, 0x00,0x01, 0xA,+0 }, // 2657: b42P73; higuiro
    { 0x0FAF320,0x22B5308, 0x00,0x0A, 0x8,+0 }, // 2658: b42P74; loguiro
    { 0x19AF815,0x089F613, 0x21,0x00, 0x8,+0 }, // 2659: b42P75; clave
    { 0x0075F20,0x14B8708, 0x01,0x00, 0x0,+0 }, // 2660: b42P78; hicuica
    { 0x1F75725,0x1677803, 0x12,0x00, 0x0,+0 }, // 2661: b42P79; locuica
    { 0x088FA21,0x097B313, 0x03,0x00, 0xC,+0 }, // 2662: b42M37; SLAPBAS2
    { 0x200FF2E,0x02D210E, 0x00,0x0E, 0xE,+0 }, // 2663: b42M119; REVRSCYM
    { 0x1E45630,0x2875517, 0x0B,0x00, 0x0,+0 }, // 2664: b42M120; FRETNOIS
    { 0x003FF24,0x1879805, 0x00,0x08, 0xC,+0 }, // 2665: b42M121; BRTHNOIS
    { 0x200F00E,0x304170A, 0x00,0x04, 0xE,+0 }, // 2666: b42M122; SEASHORE
    { 0x0F7F620,0x2F9770E, 0x08,0x05, 0x0,+0 }, // 2667: b42M123; BIRDS
    { 0x008F120,0x008F42E, 0x14,0x02, 0x0,+0 }, // 2668: b42M124; TELEPHON
    { 0x100F220,0x1053623, 0x04,0x00, 0x2,+0 }, // 2669: b42M125; HELICOPT
    { 0x002FF2E,0x355322A, 0x00,0x05, 0xE,+0 }, // 2670: b42M126; APPLAUSE
    { 0x0976800,0x3987802, 0x00,0x00, 0x0,+0 }, // 2671: b43P29; b43P30; scratch
    { 0x1FCF720,0x04AF80A, 0x00,0x00, 0x6,+0 }, // 2672: b43P32; hiq
    { 0x2F5F5C5,0x005C301, 0x08,0x06, 0x1,+0 }, // 2673: b43M9; b43P34; b44P9; GLOCKEN; GLOCKEN.; glocken
    { 0x053F600,0x07AF710, 0x0C,0x00, 0x0,+0 }, // 2674: b43P35; Kick2
    { 0x0FEF227,0x3D8980A, 0x00,0x0C, 0x8,+0 }, // 2675: b43P42; b43P44; clshat96
    { 0x0F8F128,0x3667606, 0x00,0x0A, 0xC,+0 }, // 2676: b43P46; Opnhat96
    { 0x0E5AD37,0x1A58211, 0x40,0x00, 0x0,+0 }, // 2677: b43M0; PIANO1
    { 0x053F335,0x1F5F111, 0xDA,0x03, 0x0,+0 }, // 2678: b43M1; b44P1; PIANO2; PIANO2.I
    { 0x163F435,0x1F5F211, 0xCF,0x03, 0x0,+0 }, // 2679: b43M2; b44P2; PIANO3; PIANO3.I
    { 0x163F374,0x1F5F251, 0xD3,0x03, 0x0,+0 }, // 2680: b43M3; HONKYTNK
    { 0x0F7F201,0x2C9F887, 0x06,0x15, 0x5,+0 }, // 2681: b43M4; EPIANO1
    { 0x08EF63C,0x0F5F131, 0x1B,0x09, 0x0,+0 }, // 2682: b43M5; EPIANO2
    { 0x20AFAB2,0x1F7C231, 0x15,0x05, 0xC,+0 }, // 2683: b43M6; b44P6; HARPSI; HARPSI.I
    { 0x020F831,0x1DCF236, 0x0F,0x04, 0x0,+0 }, // 2684: b43M7; CLAV
    { 0x234F825,0x085F401, 0xA2,0x07, 0x6,+0 }, // 2685: b43M8; b44P8; CELESTA; CELESTA.
    { 0x226F6C2,0x075A501, 0x05,0x05, 0x9,+0 }, // 2686: b43M10; b44P10; MUSICBOX
    { 0x131F6F5,0x0E3F1F1, 0x2A,0x02, 0x0,+0 }, // 2687: b43M11; b44P11; VIBES; VIBES.IN
    { 0x0F8F8F8,0x064E4D1, 0x1A,0x07, 0xC,+0 }, // 2688: b43M12; b44P12; MARIMBA; MARIMBA.
    { 0x0F7F73C,0x0F5F531, 0x0C,0x06, 0x9,+0 }, // 2689: b43M13; XYLOPHON
    { 0x0F0B022,0x0F4C425, 0x21,0x08, 0x0,+0 }, // 2690: b43M14; TUBEBELL
    { 0x136F8C5,0x194C311, 0x09,0x06, 0x0,+0 }, // 2691: b43M15; SANTUR
    { 0x11BF4E2,0x11DD4E0, 0x08,0x04, 0x1,+0 }, // 2692: b43M16; ORGAN1
    { 0x04CF7F2,0x00BF5F0, 0x02,0x04, 0x1,+0 }, // 2693: b43M17; b44P17; ORGAN2; ORGAN2.I
    { 0x13DF4E0,0x13BF5E0, 0x03,0x00, 0x7,+0 }, // 2694: b43M18; ORGAN3
    { 0x1166722,0x1086DE0, 0x09,0x05, 0xB,+0 }, // 2695: b43M19; b44P19; CHRCHORG
    { 0x0066331,0x1175172, 0x27,0x04, 0x0,+0 }, // 2696: b43M20; b44P20; REEDORG; REEDORG.
    { 0x11653B4,0x1175171, 0x1B,0x06, 0xE,+0 }, // 2697: b43M21; ACCORD
    { 0x1057824,0x1085333, 0x1E,0x09, 0x0,+0 }, // 2698: b43M22; b44P22; HARMO; HARMO.IN
    { 0x11653B3,0x1175172, 0x1F,0x05, 0x0,+0 }, // 2699: b43M23; b44P23; BANDNEON
    { 0x127F833,0x0F8F231, 0x23,0x04, 0xE,+0 }, // 2700: b43M24; b44P24; NYLONGT; NYLONGT.
    { 0x132F418,0x1A7E211, 0x26,0x03, 0x0,+0 }, // 2701: b43M25; STEELGT
    { 0x0C1A233,0x09CB131, 0x9D,0x85, 0x8,+0 }, // 2702: b43M26; b44P26; JAZZGT; JAZZGT.I
    { 0x1F4F335,0x1C9F232, 0x16,0x07, 0xA,+0 }, // 2703: b43M27; CLEANGT
    { 0x07B9C21,0x0FB9402, 0x12,0x03, 0xA,+0 }, // 2704: b43M28; MUTEGT
    { 0x24C8120,0x17AF126, 0x06,0x0C, 0x0,+0 }, // 2705: b43M29; OVERDGT
    { 0x28B7120,0x378F120, 0x11,0x06, 0x0,+0 }, // 2706: b43M30; DISTGT
    { 0x38C7205,0x19CE203, 0x13,0x0A, 0x4,+0 }, // 2707: b43M31; b44P31; GTHARMS; GTHARMS.
    { 0x0B6AF31,0x0F78331, 0x00,0x00, 0x0,+0 }, // 2708: b43M32; ACOUBASS
    { 0x068F321,0x0FCC121, 0x17,0x06, 0x8,+0 }, // 2709: b43M33; FINGBASS
    { 0x077FB21,0x06AC322, 0x00,0x03, 0x8,+0 }, // 2710: b43M34; PICKBASS
    { 0x047A131,0x0878231, 0x97,0x84, 0xA,+0 }, // 2711: b43M35; FRETLESS
    { 0x0A8FA25,0x197F312, 0x0D,0x00, 0x8,+0 }, // 2712: b43M36; SLAPBAS1
    { 0x06CFA21,0x0FCF334, 0x05,0x07, 0xC,+0 }, // 2713: b43M37; SLAPBAS2
    { 0x17FF521,0x0CCF322, 0x17,0x03, 0xE,+0 }, // 2714: b43M38; SYNBASS1
    { 0x09BA301,0x0AA9301, 0x13,0x04, 0xA,+0 }, // 2715: b43M39; SYNBASS2
    { 0x129F6E2,0x10878E1, 0x19,0x05, 0xC,+0 }, // 2716: b43M40; b44P40; VIOLIN; VIOLIN.I
    { 0x129F6E2,0x10878E1, 0x1C,0x03, 0xC,+0 }, // 2717: b43M41; VIOLA
    { 0x0099861,0x1087E61, 0x20,0x03, 0xC,+0 }, // 2718: b43M42; b44P42; CELLO; CELLO.IN
    { 0x1017171,0x05651F1, 0x1E,0x06, 0xE,+0 }, // 2719: b43M43; b44P43; CONTRAB; CONTRAB.
    { 0x10670E2,0x11675E1, 0x23,0x04, 0xC,+0 }, // 2720: b43M44; b44P44; TREMSTR; TREMSTR.
    { 0x0E69802,0x0F6F521, 0x05,0x07, 0x9,+0 }, // 2721: b43M45; PIZZ
    { 0x075F602,0x0C5F401, 0x2A,0x82, 0xE,+0 }, // 2722: b43M46; b44P46; HARP; HARP.INS
    { 0x1BABF61,0x0468501, 0x40,0x00, 0x0,+0 }, // 2723: b43M47; TIMPANI
    { 0x195CCE1,0x12850E1, 0x00,0x00, 0x0,+0 }, // 2724: b43M48; STRINGS
    { 0x2D6C0E2,0x15530E1, 0x27,0x09, 0xE,+0 }, // 2725: b43M49; b44P49; SLOWSTR; SLOWSTR.
    { 0x1556261,0x1566261, 0x26,0x03, 0xE,+0 }, // 2726: b43M50; b44P50; SYNSTR1; SYNSTR1.
    { 0x16372A1,0x00751A1, 0x18,0x07, 0xE,+0 }, // 2727: b43M51; SYNSTR2
    { 0x001D3E1,0x0396262, 0xCA,0x83, 0x6,+0 }, // 2728: b43M52; b44P52; CHOIR; CHOIR.IN
    { 0x145B822,0x0278621, 0xD2,0x02, 0x0,+0 }, // 2729: b43M53; b44P53; OOHS; OOHS.INS
    { 0x1556321,0x0467321, 0xDE,0x05, 0x0,+0 }, // 2730: b43M54; b44P54; SYNVOX; SYNVOX.I
    { 0x0F78642,0x1767450, 0x0A,0x00, 0xD,+0 }, // 2731: b43M55; b44P55; ORCHIT; ORCHIT.I
    { 0x0026131,0x0388261, 0x1F,0x87, 0xE,+0 }, // 2732: b43M56; TRUMPET
    { 0x0135571,0x0197061, 0x20,0x0B, 0xE,+0 }, // 2733: b43M57; TROMBONE
    { 0x0166621,0x0097121, 0x1C,0x06, 0xE,+0 }, // 2734: b43M58; TUBA
    { 0x21C7824,0x14B9321, 0x19,0x84, 0x0,+0 }, // 2735: b43M59; b44P59; MUTETRP; MUTETRP.
    { 0x0167921,0x05971A1, 0x21,0x03, 0xC,+0 }, // 2736: b43M60; FRHORN
    { 0x0358221,0x0388221, 0x1B,0x07, 0xE,+0 }, // 2737: b43M61; TCBRASS1
    { 0x0357221,0x0378222, 0x1A,0x87, 0xE,+0 }, // 2738: b43M62; SYNBRAS1
    { 0x0586221,0x0167221, 0x23,0x06, 0xE,+0 }, // 2739: b43M63; b44P63; SYNBRAS2
    { 0x10759F1,0x00A7B61, 0x1B,0x06, 0x0,+0 }, // 2740: b43M64; b44P64; SOPSAX; SOPSAX.I
    { 0x0049F21,0x10C8521, 0x16,0x07, 0xA,+0 }, // 2741: b43M65; b44P65; ALTOSAX; ALTOSAX.
    { 0x010B821,0x1DC72A6, 0x04,0x04, 0x8,+0 }, // 2742: b43M66; b44P66; TENSAX; TENSAX.I
    { 0x0096831,0x1086334, 0x0B,0x09, 0x6,+0 }, // 2743: b43M67; b44P67; BARISAX; BARISAX.
    { 0x1058F31,0x00B5333, 0x14,0x16, 0x0,+0 }, // 2744: b43M68; OBOE
    { 0x1079FA1,0x00A7724, 0x1D,0x08, 0xA,+0 }, // 2745: b43M69; b44P69; ENGLHORN
    { 0x009D531,0x01D6175, 0x1B,0x4C, 0xA,+0 }, // 2746: b43M70; BASSOON
    { 0x0076172,0x01B6223, 0x26,0x10, 0xE,+0 }, // 2747: b43M71; CLARINET
    { 0x194A8E1,0x0086221, 0x0F,0x04, 0x0,+0 }, // 2748: b43M72; b44P72; PICCOLO; PICCOLO.
    { 0x00986F1,0x00B75E1, 0x9C,0x0B, 0x0,+0 }, // 2749: b43M73; FLUTE1
    { 0x008DF22,0x0297761, 0x2C,0x03, 0x0,+0 }, // 2750: b43M74; b44P74; RECORDER
    { 0x27A88E2,0x0097721, 0x2C,0x00, 0x0,+0 }, // 2751: b43M75; b44P75; PANFLUTE
    { 0x05488E2,0x0087721, 0x17,0x0B, 0xE,+0 }, // 2752: b43M76; BOTTLEB
    { 0x02686F1,0x02755F1, 0x1F,0x04, 0xE,+0 }, // 2753: b43M77; SHAKU
    { 0x0099FE1,0x0086FE1, 0x3F,0x05, 0x1,+0 }, // 2754: b43M78; WHISTLE
    { 0x004A822,0x0096A21, 0xE6,0x05, 0x0,+0 }, // 2755: b43M79; OCARINA
    { 0x00C9222,0x00DA261, 0x1B,0x0A, 0xE,+0 }, // 2756: b43M80; SQUARWAV
    { 0x122F461,0x05FA361, 0x15,0x04, 0xE,+0 }, // 2757: b43M81; SAWWAV
    { 0x10ABB21,0x0096FA1, 0xD2,0x03, 0xC,+0 }, // 2758: b43M82; b44P82; SYNCALLI
    { 0x0387761,0x0499261, 0x17,0x09, 0x8,+0 }, // 2759: b43M83; b44P83; CHIFLEAD
    { 0x21D7120,0x178F124, 0x08,0x05, 0x0,+0 }, // 2760: b43M84; CHARANG
    { 0x193CA21,0x01A7A21, 0x00,0x03, 0x0,+0 }, // 2761: b43M85; b44P85; SOLOVOX; SOLOVOX.
    { 0x1C99223,0x1089122, 0x06,0x08, 0xB,+0 }, // 2762: b43M86; FIFTHSAW
    { 0x01BF321,0x05FE122, 0x1D,0x04, 0xE,+0 }, // 2763: b43M87; BASSLEAD
    { 0x15562E1,0x125FAC8, 0x01,0x0B, 0x5,+0 }, // 2764: b43M88; b44P88; FANTASIA
    { 0x0012161,0x01534E1, 0x26,0x02, 0xE,+0 }, // 2765: b43M89; WARMPAD
    { 0x0358361,0x106D161, 0x19,0x02, 0xC,+0 }, // 2766: b43M90; POLYSYN
    { 0x101D3E1,0x0378262, 0xDC,0x82, 0x0,+0 }, // 2767: b43M91; SPACEVOX
    { 0x166446A,0x0365161, 0x33,0x04, 0x0,+0 }, // 2768: b43M92; BOWEDGLS
    { 0x0F38262,0x1F53261, 0x0B,0x06, 0x4,+0 }, // 2769: b43M93; METALPAD
    { 0x1766261,0x02661A1, 0x9A,0x04, 0xC,+0 }, // 2770: b43M94; HALOPAD
    { 0x1D52222,0x1053F21, 0x13,0x06, 0xA,+0 }, // 2771: b43M95; SWEEPPAD
    { 0x0F4F2E1,0x0F69121, 0x9C,0x05, 0xE,+0 }, // 2772: b43M96; b44P96; ICERAIN; ICERAIN.
    { 0x1554163,0x10541A2, 0x0A,0x06, 0xB,+0 }, // 2773: b43M97; SOUNDTRK
    { 0x005F604,0x0E5F301, 0x18,0x0E, 0x0,+0 }, // 2774: b43M98; CRYSTAL
    { 0x196F9E3,0x1F5C261, 0x10,0x00, 0x8,+0 }, // 2775: b43M99; ATMOSPH
    { 0x1C6A144,0x1E5B241, 0xD2,0x06, 0xE,+0 }, // 2776: b43M100; BRIGHT
    { 0x1772261,0x0264561, 0x94,0x05, 0xE,+0 }, // 2777: b43M101; b44P101; GOBLIN; GOBLIN.I
    { 0x184F5E1,0x036A2E1, 0x19,0x07, 0xE,+0 }, // 2778: b43M102; ECHODROP
    { 0x16473E2,0x10598E1, 0x14,0x07, 0xA,+0 }, // 2779: b43M103; STARTHEM
    { 0x0348321,0x1F6C324, 0x0B,0x09, 0x8,+0 }, // 2780: b43M104; b44P104; SITAR; SITAR.IN
    { 0x19AFB25,0x1F7F432, 0x00,0x03, 0x0,+0 }, // 2781: b43M105; BANJO
    { 0x002DA21,0x0F5F335, 0x1B,0x04, 0xC,+0 }, // 2782: b43M106; SHAMISEN
    { 0x034F763,0x1E5F301, 0x4E,0x05, 0x0,+0 }, // 2783: b43M107; b44P107; KOTO; KOTO.INS
    { 0x296F931,0x0F6F531, 0x0F,0x04, 0xA,+0 }, // 2784: b43M108; b44P108; KALIMBA; KALIMBA.
    { 0x1176731,0x01A7325, 0x17,0x0A, 0xE,+0 }, // 2785: b43M109; b44P109; BAGPIPE; BAGPIPE.
    { 0x129F6E1,0x20868E2, 0x15,0x07, 0x0,+0 }, // 2786: b43M110; b44P110; FIDDLE; FIDDLE.I
    { 0x019A6E6,0x1088E61, 0x23,0x05, 0x0,+0 }, // 2787: b43M111; SHANNAI
    { 0x0D4F027,0x046F205, 0x23,0x0C, 0x0,+0 }, // 2788: b43M112; b44P112; TINKLBEL
    { 0x1167504,0x1F6C601, 0x07,0x00, 0x5,+0 }, // 2789: b43M114; STEELDRM
    { 0x033F731,0x085F510, 0x19,0x00, 0x0,+0 }, // 2790: b43M117; MELOTOM
    { 0x089FA22,0x025F501, 0x0F,0x05, 0xE,+0 }, // 2791: b43M118; SYNDRUM
    { 0x200FF2E,0x02D210E, 0x00,0x18, 0xE,+0 }, // 2792: b43M119; REVRSCYM
    { 0x0F45630,0x2875517, 0x00,0x00, 0x8,+0 }, // 2793: b43M120; b44P120; FRETNOIS
    { 0x003FF20,0x3967604, 0x00,0x06, 0xE,+0 }, // 2794: b43M121; BRTHNOIS
    { 0x200F00E,0x304170A, 0x00,0x13, 0xE,+0 }, // 2795: b43M122; SEASHORE
    { 0x007F020,0x2F9920E, 0x0C,0x08, 0x0,+0 }, // 2796: b43M123; b44P123; BIRDS; BIRDS.IN
    { 0x008F120,0x008F42E, 0x14,0x08, 0x0,+0 }, // 2797: b43M124; b44P124; TELEPHON
    { 0x100F220,0x0052423, 0x09,0x05, 0xE,+0 }, // 2798: b43M125; HELICOPT
    { 0x002FF2E,0x325332E, 0x00,0x0A, 0xE,+0 }, // 2799: b43M126; APPLAUSE
    { 0x0DF8120,0x0DFF310, 0x00,0x03, 0xE,+0 }, // 2800: b43M127; GUNSHOT
    { 0x0F9F700,0x0CA8601, 0x08,0x00, 0x0,+0 }, // 2801: b44M36; Kick.ins
    { 0x091F010,0x0E7A51E, 0x0C,0x00, 0x0,+0 }, // 2802: b44M67; b44M68; b44P98; CRYSTAL.; agogo.in
    { 0x050F335,0x1F5F111, 0x69,0x02, 0x0,+0 }, // 2803: b44P0; PIANO1.I
    { 0x2B49230,0x208A421, 0x0F,0x00, 0xC,+0 }, // 2804: b44P3; TCSAWWAV
    { 0x0A7FB2C,0x0C9F281, 0x16,0x08, 0x0,+0 }, // 2805: b44P4; EPIANO1.
    { 0x08EA43A,0x085A131, 0x35,0x07, 0xC,+0 }, // 2806: b44P5; EPIANO2.
    { 0x010A831,0x1B9D234, 0x0A,0x03, 0x6,+0 }, // 2807: b44P7; TCCLAV.I
    { 0x0F7F838,0x0F5F537, 0x13,0x06, 0x8,+0 }, // 2808: b44P13; XYLOPHON
    { 0x061C21A,0x072C212, 0x18,0x03, 0x6,+0 }, // 2809: b44P14; TCBELL.I
    { 0x136F8C2,0x194C311, 0x03,0x03, 0x0,+0 }, // 2810: b44P15; SANTUR.I
    { 0x34FFAE1,0x11AD4E0, 0x07,0x07, 0x1,+0 }, // 2811: b44P16; ORGAN1.I
    { 0x13DF9E3,0x03BF5E0, 0x00,0x00, 0x0,+0 }, // 2812: b44P18; ORGAN3.I
    { 0x1F62334,0x1173131, 0x1E,0x06, 0xE,+0 }, // 2813: b44P21; ACCORD.I
    { 0x1F2F235,0x1A7E211, 0x02,0x03, 0x0,+0 }, // 2814: b44P25; STEELGT.
    { 0x084FA37,0x1C9F232, 0x09,0x00, 0x0,+0 }, // 2815: b44P27; CLEANGT.
    { 0x3CEFA21,0x0FBF403, 0x03,0x00, 0x0,+0 }, // 2816: b44P28; MUTEGT.I
    { 0x2989120,0x159B125, 0x06,0x06, 0x0,+0 }, // 2817: b44P29; TCOVRDGT
    { 0x073F9A1,0x3FCA120, 0x0D,0x04, 0xA,+0 }, // 2818: b44P30; TCDISTG2
    { 0x036F821,0x0F7C123, 0x11,0x00, 0x8,+0 }, // 2819: b44P32; ACOUBASS
    { 0x017F821,0x0FAF223, 0x9E,0x00, 0xE,+0 }, // 2820: b44P33; FINGBASS
    { 0x146F821,0x006C322, 0x0C,0x07, 0x6,+0 }, // 2821: b44P34; PICKBASS
    { 0x047F531,0x087F233, 0x96,0x80, 0xA,+0 }, // 2822: b44P35; FRETLESS
    { 0x0B8FA21,0x077F412, 0x04,0x07, 0x0,+0 }, // 2823: b44P36; SLAPBAS1
    { 0x08CF921,0x0FCF334, 0x05,0x00, 0x0,+0 }, // 2824: b44P37; SLAPBAS2
    { 0x066F801,0x1F6F521, 0x08,0x06, 0x8,+0 }, // 2825: b44P38; SYNBASS1
    { 0x09BF501,0x0AAF302, 0x19,0x04, 0xC,+0 }, // 2826: b44P39; SYNBASS2
    { 0x124F661,0x2065860, 0x17,0x0B, 0xE,+0 }, // 2827: b44P41; VIOLA.IN
    { 0x006F701,0x3F6F720, 0x19,0x08, 0xE,+0 }, // 2828: b44P45; PIZZ.INS
    { 0x1F4F461,0x0F5B500, 0x14,0x00, 0x0,+0 }, // 2829: b44P47; TIMPANI.
    { 0x104F6E1,0x12670E1, 0x23,0x05, 0xE,+0 }, // 2830: b44P48; STRINGS.
    { 0x113F221,0x0055121, 0x20,0x09, 0xE,+0 }, // 2831: b44P51; SYNSTR2.
    { 0x0026131,0x0388261, 0x1F,0x83, 0xE,+0 }, // 2832: b44P56; TRUMPET.
    { 0x0135571,0x0197061, 0x20,0x06, 0xE,+0 }, // 2833: b44P57; TROMBONE
    { 0x0157121,0x0177122, 0x1C,0x00, 0xE,+0 }, // 2834: b44P58; TUBA.INS
    { 0x0257521,0x01771A1, 0x21,0x00, 0xC,+0 }, // 2835: b44P60; FRHORN2.
    { 0x0358221,0x0388221, 0x19,0x03, 0xE,+0 }, // 2836: b44P61; TCBRASS1
    { 0x0357221,0x0378222, 0x1A,0x82, 0xE,+0 }, // 2837: b44P62; SYNBRAS1
    { 0x1058F31,0x0085333, 0x14,0x0A, 0x0,+0 }, // 2838: b44P68; OBOE.INS
    { 0x009D531,0x01B6175, 0x1B,0x84, 0xA,+0 }, // 2839: b44P70; BASSOON.
    { 0x0076172,0x0186223, 0x26,0x0A, 0xE,+0 }, // 2840: b44P71; CLARINET
    { 0x00986F1,0x00A75E1, 0x9C,0x05, 0x0,+0 }, // 2841: b44P73; FLUTE1.I
    { 0x02384F1,0x01655F2, 0x1D,0x00, 0xE,+0 }, // 2842: b44P76; SHAKU.IN
    { 0x2D86901,0x0B65701, 0x1B,0x00, 0xC,+0 }, // 2843: b44P77; TCCHIFF.
    { 0x0C4FF22,0x0077921, 0x00,0x0D, 0x0,+0 }, // 2844: b44P79; OCARINA.
    { 0x05FB9A2,0x0FB9121, 0x0B,0x0F, 0xE,+0 }, // 2845: b44P80; SQUARWAV
    { 0x072FA62,0x198F541, 0x09,0x00, 0xC,+0 }, // 2846: b44P81; SAWWAV.I
    { 0x21D8120,0x179F125, 0x08,0x05, 0x0,+0 }, // 2847: b44P84; CHARANG.
    { 0x1C99223,0x1089122, 0x0C,0x0E, 0xD,+0 }, // 2848: b44P86; FIFTHSAW
    { 0x01BF321,0x05FE121, 0x1D,0x0A, 0xE,+0 }, // 2849: b44P87; BASSLEAD
    { 0x001F1A1,0x0153421, 0x27,0x07, 0xE,+0 }, // 2850: b44P89; WARMPAD.
    { 0x2A2F120,0x315F321, 0x14,0x12, 0x0,+0 }, // 2851: b44P90; POLYSYN.
    { 0x034D2E8,0x1343261, 0xDD,0x8B, 0x0,+0 }, // 2852: b44P91; SPACEVOX
    { 0x053F265,0x1F33263, 0x0E,0x11, 0x0,+0 }, // 2853: b44P92; BOWEDGLS
    { 0x0837222,0x1055221, 0x19,0x05, 0xC,+0 }, // 2854: b44P93; METALPAD
    { 0x074F161,0x07441A1, 0x22,0x06, 0xE,+0 }, // 2855: b44P94; HALOPAD.
    { 0x00553A1,0x0F43221, 0x25,0x00, 0xE,+0 }, // 2856: b44P95; SWEEPPAD
    { 0x1554163,0x10541A2, 0x0A,0x03, 0xB,+0 }, // 2857: b44P97; SOUNDTRK
    { 0x2B29130,0x204A121, 0x10,0x00, 0xC,+0 }, // 2858: b44P99; TCSYNTH1
    { 0x0D6F662,0x2E5B241, 0x22,0x00, 0xE,+0 }, // 2859: b44P100; BRIGHT.I
    { 0x104F021,0x0043221, 0x2B,0x06, 0xE,+0 }, // 2860: b44P102; ECHODROP
    { 0x06473E4,0x10548E1, 0x25,0x08, 0x0,+0 }, // 2861: b44P103; STARTHEM
    { 0x156FA23,0x0FBF622, 0x00,0x00, 0x0,+0 }, // 2862: b44P105; BANJO.IN
    { 0x28CFA21,0x1F7F331, 0x13,0x04, 0xC,+0 }, // 2863: b44P106; SHAMISEN
    { 0x0559131,0x3788133, 0x0D,0x02, 0xA,+0 }, // 2864: b44P111; TCPAD1.I
    { 0x0411160,0x14431E6, 0x05,0x00, 0x8,+0 }, // 2865: b44P113; TCLOWPAD
    { 0x0722121,0x2646129, 0x0D,0x0D, 0x4,+0 }, // 2866: b44P114; TCPAD4.I
    { 0x3922220,0x0A44125, 0x84,0x82, 0x8,+0 }, // 2867: b44P117; TCPAD7.I
    { 0x1023220,0x3343120, 0x03,0x00, 0xC,+0 }, // 2868: b44P118; TCPAD8.I
    { 0x0B5F100,0x0C2D400, 0x0B,0x07, 0xA,+0 }, // 2869: b44P119; TCSFX1.I
    { 0x300FF36,0x2F4F41E, 0x09,0x00, 0xE,+0 }, // 2870: b44P121; BRTHNOIS
    { 0x0211131,0x0937122, 0x0A,0x02, 0xA,+0 }, // 2871: b44P122; TCPAD2.I
    { 0x1728281,0x0743182, 0x0E,0x05, 0xC,+0 }, // 2872: b44P125; TCPAD5.I
    { 0x0331221,0x1243122, 0x00,0x00, 0x8,+0 }, // 2873: b44P126; TCPAD6.I
    { 0x00AAFE1,0x00AAF62, 0x91,0x83, 0x9,-12 }, // 2874: b46M18; b47M18; gm018
    { 0x02661B1,0x0266171, 0xD3,0x80, 0xD,+12 }, // 2875: b46M58; b47M58; gm058
    { 0x035C100,0x0D5C111, 0x9B,0x00, 0xC,+12 }, // 2876: b46M3; b47M3; gm003
    { 0x040B230,0x5E9F111, 0xA2,0x80, 0x4,+12 }, // 2877: b46M7; b47M7; gm007
    { 0x0E6F314,0x0E6F280, 0x62,0x00, 0xB,+12 }, // 2878: b46M11; b47M11; gm011
    { 0x715FE11,0x019F487, 0x20,0xC0, 0xB,+1 }, // 2879: b46M124; b47M124; gm124
    { 0x7112EF0,0x11621E2, 0x00,0xC0, 0x9,-36 }, // 2880: b46M125; b47M125; gm125
    { 0x00CF600,0x00CF600, 0x00,0x00, 0x1,+0 }, // 2881: b46P36; b46P43; b46P45; b46P47; b46P48; b46P50; gps036; gps043; gps045; gps047; gps048; gps050
    { 0x001FF26,0x3751304, 0x00,0x00, 0xE,+0 }, // 2882: b47P88; gpo088
    { 0x0E5F108,0x0E5C302, 0x66,0x86, 0x8,+5 }, // 2883: b47P83; gpo083
    { 0x052F605,0x0D5F582, 0x69,0x47, 0x9,+5 }, // 2884: b47P83; gpo083
    { 0x6E5E403,0x7E7F507, 0x0D,0x11, 0xB,+12 }, // 2885: b47P84; gpo084
    { 0x366F500,0x4A8F604, 0x1B,0x15, 0xA,+12 }, // 2886: b47P84; gpo084
};
const struct adlinsdata adlins[2811] =
{
    // Amplitude begins at 2487.8, peaks 3203.1 at 0.1s,
    // fades to 20% at 1.6s, keyoff fades to 20% in 1.6s.
    {   0,  0,  0,   1560,  1560 }, // 0: GM0; b45M0; f29GM0; f30GM0; sGM0; AcouGrandPiano; am000

    // Amplitude begins at 3912.6,
    // fades to 20% at 2.1s, keyoff fades to 20% in 2.1s.
    {   1,  1,  0,   2080,  2080 }, // 1: GM1; b45M1; f29GM1; f30GM1; sGM1; BrightAcouGrand; am001

    // Amplitude begins at 2850.7, peaks 4216.6 at 0.0s,
    // fades to 20% at 2.0s, keyoff fades to 20% in 2.0s.
    {   2,  2,  0,   1973,  1973 }, // 2: GM2; b45M2; f29GM2; f30GM2; f34GM0; f34GM1; f34GM2; sGM2; AcouGrandPiano; BrightAcouGrand; ElecGrandPiano; am002

    // Amplitude begins at 1714.3, peaks 1785.0 at 0.1s,
    // fades to 20% at 2.2s, keyoff fades to 20% in 2.2s.
    {   3,  3,  0,   2226,  2226 }, // 3: GM3; b45M3; f34GM3; sGM3; Honky-tonkPiano; am003

    // Amplitude begins at 4461.0, peaks 6341.0 at 0.0s,
    // fades to 20% at 2.6s, keyoff fades to 20% in 2.6s.
    {   4,  4,  0,   2606,  2606 }, // 4: GM4; b45M4; f34GM4; sGM4; Rhodes Piano; am004

    // Amplitude begins at 4781.0, peaks 6329.2 at 0.0s,
    // fades to 20% at 2.6s, keyoff fades to 20% in 2.6s.
    {   5,  5,  0,   2640,  2640 }, // 5: GM5; b45M5; f29GM6; f30GM6; f34GM5; sGM5; Chorused Piano; Harpsichord; am005

    // Amplitude begins at 1162.2, peaks 1404.5 at 0.0s,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    {   6,  6,  0,   1186,  1186 }, // 6: GM6; b45M6; f34GM6; Harpsichord; am006

    // Amplitude begins at 1144.6, peaks 1235.5 at 0.0s,
    // fades to 20% at 2.3s, keyoff fades to 20% in 2.3s.
    {   7,  7,  0,   2313,  2313 }, // 7: GM7; b45M7; f34GM7; sGM7; Clavinet; am007

    // Amplitude begins at 2803.9, peaks 2829.0 at 0.0s,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.9s.
    {   8,  8,  0,    906,   906 }, // 8: GM8; b45M8; f34GM8; sGM8; Celesta; am008

    // Amplitude begins at 3085.2, peaks 3516.8 at 0.0s,
    // fades to 20% at 1.1s, keyoff fades to 20% in 1.1s.
    {   9,  9,  0,   1120,  1120 }, // 9: GM9; b45M9; f29GM101; f30GM101; f34GM9; FX 6 goblins; Glockenspiel; am009

    // Amplitude begins at 2073.6, peaks 3449.1 at 0.1s,
    // fades to 20% at 0.5s, keyoff fades to 20% in 0.5s.
    {  10, 10,  0,    453,   453 }, // 10: GM10; b45M10; f29GM100; f30GM100; f34GM10; sGM10; FX 5 brightness; Music box; am010

    // Amplitude begins at 2976.7, peaks 3033.0 at 0.0s,
    // fades to 20% at 1.7s, keyoff fades to 20% in 1.7s.
    {  11, 11,  0,   1746,  1746 }, // 11: GM11; b45M11; f34GM11; sGM11; Vibraphone; am011

    // Amplitude begins at 3371.2, peaks 3554.9 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    {  12, 12,  0,     80,    80 }, // 12: GM12; b45M12; f29GM104; f29GM97; f30GM104; f30GM97; f34GM12; sGM12; FX 2 soundtrack; Marimba; Sitar; am012

    // Amplitude begins at 2959.7, peaks 3202.7 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    {  13, 13,  0,    100,   100 }, // 13: GM13; b45M13; f29GM103; f30GM103; f34GM13; sGM13; FX 8 sci-fi; Xylophone; am013

    // Amplitude begins at 2057.2, peaks 2301.4 at 0.0s,
    // fades to 20% at 1.3s, keyoff fades to 20% in 1.3s.
    {  14, 14,  0,   1306,  1306 }, // 14: GM14; b45M14; f34GM14; Tubular Bells; am014

    // Amplitude begins at 1672.7, peaks 2154.8 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    {  15, 15,  0,    320,   320 }, // 15: GM15; b45M15; f34GM15; sGM15; Dulcimer; am015

    // Amplitude begins at 2324.3, peaks 2396.0 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    {  16, 16,  0,  40000,    40 }, // 16: GM16; HMIGM16; b45M16; f34GM16; sGM16; Hammond Organ; am016; am016.in

    // Amplitude begins at 2299.6, peaks 2620.0 at 14.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    {  17, 17,  0,  40000,    20 }, // 17: GM17; HMIGM17; b45M17; f34GM17; sGM17; Percussive Organ; am017; am017.in

    // Amplitude begins at  765.8, peaks 2166.1 at 4.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    {  18, 18,  0,  40000,    20 }, // 18: GM18; HMIGM18; b45M18; f34GM18; sGM18; Rock Organ; am018; am018.in

    // Amplitude begins at  336.5, peaks 2029.7 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.5s.
    {  19, 19,  0,  40000,   500 }, // 19: GM19; HMIGM19; b45M19; f34GM19; Church Organ; am019; am019.in

    // Amplitude begins at  153.2, peaks 4545.1 at 39.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.3s.
    {  20, 20,  0,  40000,   280 }, // 20: GM20; HMIGM20; b45M20; f34GM20; sGM20; Reed Organ; am020; am020.in

    // Amplitude begins at    0.0, peaks  883.0 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    {  21, 21,  0,  40000,     6 }, // 21: GM21; HMIGM21; b45M21; f34GM21; sGM21; Accordion; am021; am021.in

    // Amplitude begins at  181.4, peaks 3015.8 at 25.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    {  22, 22,  0,  40000,    86 }, // 22: GM22; HMIGM22; b45M22; f34GM22; sGM22; Harmonica; am022; am022.in

    // Amplitude begins at    3.2, peaks 3113.2 at 39.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    {  23, 23,  0,  40000,    73 }, // 23: GM23; HMIGM23; b45M23; f34GM23; sGM23; Tango Accordion; am023; am023.in

    // Amplitude begins at  955.9, peaks 1147.3 at 0.0s,
    // fades to 20% at 1.1s, keyoff fades to 20% in 1.1s.
    {  24, 24,  0,   1120,  1120 }, // 24: GM24; HMIGM24; b45M24; f34GM24; Acoustic Guitar1; am024; am024.in

    // Amplitude begins at 1026.8, peaks 1081.7 at 0.0s,
    // fades to 20% at 2.0s, keyoff fades to 20% in 2.0s.
    {  25, 25,  0,   1973,  1973 }, // 25: GM25; HMIGM25; b45M25; f17GM25; f29GM60; f30GM60; f34GM25; mGM25; sGM25; Acoustic Guitar2; French Horn; am025; am025.in

    // Amplitude begins at 4157.9, peaks 4433.1 at 0.1s,
    // fades to 20% at 10.3s, keyoff fades to 20% in 10.3s.
    {  26, 26,  0,  10326, 10326 }, // 26: GM26; HMIGM26; b45M26; f17GM26; f34GM26; f35GM26; mGM26; sGM26; Electric Guitar1; am026; am026.in

    // Amplitude begins at 2090.6,
    // fades to 20% at 1.3s, keyoff fades to 20% in 1.3s.
    {  27, 27,  0,   1266,  1266 }, // 27: GM27; b45M27; f30GM61; f34GM27; sGM27; Brass Section; Electric Guitar2; am027

    // Amplitude begins at 3418.1,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    {  28, 28,  0,  40000,     0 }, // 28: GM28; HMIGM28; b45M28; f17GM28; f34GM28; f35GM28; hamM3; hamM60; intM3; mGM28; rickM3; sGM28; BPerc; BPerc.in; Electric Guitar3; am028; am028.in; muteguit

    // Amplitude begins at   57.7, peaks 1476.7 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    {  29, 29,  0,  40000,    13 }, // 29: GM29; b45M29; f34GM29; sGM29; Overdrive Guitar; am029

    // Amplitude begins at  396.9, peaks 1480.5 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    {  30, 30,  0,  40000,    13 }, // 30: GM30; HMIGM30; b45M30; f17GM30; f34GM30; f35GM30; hamM6; intM6; mGM30; rickM6; sGM30; Distorton Guitar; GDist; GDist.in; am030; am030.in

    // Amplitude begins at 1424.2, peaks 2686.1 at 1.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    {  31, 31,  0,  40000,     0 }, // 31: GM31; HMIGM31; b45M31; f34GM31; hamM5; intM5; rickM5; sGM31; GFeedbck; Guitar Harmonics; am031; am031.in

    // Amplitude begins at 2281.6, peaks 2475.5 at 0.0s,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    {  32, 32,  0,   1153,  1153 }, // 32: GM32; HMIGM32; b45M32; f34GM32; sGM32; Acoustic Bass; am032; am032.in

    // Amplitude begins at 1212.1, peaks 1233.4 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    {  33, 33,  0,  40000,    13 }, // 33: GM33; GM39; HMIGM33; HMIGM39; b45M33; b45M39; f15GM30; f17GM33; f17GM39; f26GM30; f29GM28; f29GM29; f30GM28; f30GM29; f34GM33; f34GM39; f35GM39; hamM68; mGM33; mGM39; sGM33; sGM39; Distorton Guitar; Electric Bass 1; Electric Guitar3; Overdrive Guitar; Synth Bass 2; am033; am033.in; am039; am039.in; synbass2

    // Amplitude begins at 3717.2, peaks 4282.2 at 0.1s,
    // fades to 20% at 1.5s, keyoff fades to 20% in 1.5s.
    {  34, 34,  0,   1540,  1540 }, // 34: GM34; HMIGM34; b45M34; f15GM28; f17GM34; f26GM28; f29GM38; f29GM67; f30GM38; f30GM67; f34GM34; f35GM37; mGM34; rickM81; sGM34; Baritone Sax; Electric Bass 2; Electric Guitar3; Slap Bass 2; Slapbass; Synth Bass 1; am034; am034.in

    // Amplitude begins at   49.0, peaks 3572.0 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    {  35, 35,  0,  40000,     0 }, // 35: GM35; HMIGM35; b45M35; f17GM35; f29GM42; f29GM70; f29GM71; f30GM42; f30GM70; f30GM71; f34GM35; mGM35; sGM35; Bassoon; Cello; Clarinet; Fretless Bass; am035; am035.in

    // Amplitude begins at 1755.7, peaks 2777.7 at 0.0s,
    // fades to 20% at 3.6s, keyoff fades to 20% in 3.6s.
    {  36, 36,  0,   3566,  3566 }, // 36: GM36; HMIGM36; b45M36; f17GM36; f29GM68; f30GM68; f34GM36; mGM36; sGM36; Oboe; Slap Bass 1; am036; am036.in

    // Amplitude begins at 1352.0, peaks 2834.3 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    {  37, 37,  0,  40000,    53 }, // 37: GM37; b45M37; f29GM69; f30GM69; f34GM37; sGM37; English Horn; Slap Bass 2; am037

    // Amplitude begins at 3787.6,
    // fades to 20% at 0.5s, keyoff fades to 20% in 0.5s.
    {  38, 38,  0,    540,   540 }, // 38: GM38; HMIGM38; b45M38; f17GM38; f29GM30; f29GM31; f30GM30; f30GM31; f34GM38; f35GM38; hamM13; hamM67; intM13; mGM38; rickM13; sGM38; BSynth3; BSynth3.; Distorton Guitar; Guitar Harmonics; Synth Bass 1; am038; am038.in; synbass1

    // Amplitude begins at 1114.5, peaks 2586.6 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    {  39, 39,  0,  40000,   106 }, // 39: GM40; HMIGM40; b45M40; f17GM40; f34GM40; mGM40; sGM40; Violin; am040; am040.in

    // Amplitude begins at    0.5, peaks 1262.2 at 16.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    {  40, 40,  0,  40000,   126 }, // 40: GM41; HMIGM41; b45M41; f17GM41; f34GM41; mGM41; sGM41; Viola; am041; am041.in

    // Amplitude begins at 1406.9, peaks 2923.1 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    {  41, 41,  0,  40000,     6 }, // 41: GM42; HMIGM42; b45M42; f34GM42; sGM42; Cello; am042; am042.in

    // Amplitude begins at    5.0, peaks 2928.6 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    {  42, 42,  0,  40000,   193 }, // 42: GM43; HMIGM43; b45M43; f17GM43; f29GM56; f30GM56; f34GM43; f35GM43; mGM43; sGM43; Contrabass; Trumpet; am043; am043.in

    // Amplitude begins at    0.6, peaks 1972.9 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    {  43, 43,  0,  40000,   106 }, // 43: GM44; HMIGM44; b45M44; f17GM44; f34GM44; f35GM44; mGM44; Tremulo Strings; am044; am044.in

    // Amplitude begins at  123.0, peaks 2834.7 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    {  44, 44,  0,    326,   326 }, // 44: GM45; HMIGM45; b45M45; f17GM45; f29GM51; f30GM51; f34GM45; mGM45; Pizzicato String; SynthStrings 2; am045; am045.in

    // Amplitude begins at 2458.2, peaks 3405.7 at 0.0s,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.9s.
    {  45, 45,  0,    853,   853 }, // 45: GM46; HMIGM46; b45M46; f15GM57; f15GM58; f17GM46; f26GM57; f26GM58; f29GM57; f29GM58; f30GM57; f30GM58; f34GM46; mGM46; oGM57; oGM58; Orchestral Harp; Trombone; Tuba; am046; am046.in

    // Amplitude begins at 2061.3, peaks 2787.1 at 0.1s,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.9s.
    {  46, 46,  0,    906,   906 }, // 46: GM47; HMIGM47; b45M47; f17GM47; f30GM112; f34GM47; hamM14; intM14; mGM47; rickM14; BSynth4; BSynth4.; Timpany; Tinkle Bell; am047; am047.in

    // Amplitude begins at  845.0, peaks 1851.0 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    {  47, 47,  0,  40000,    66 }, // 47: GM48; HMIGM48; b45M48; f34GM48; String Ensemble1; am048; am048.in

    // Amplitude begins at    0.0, peaks 1531.6 at 0.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    {  48, 48,  0,  40000,    40 }, // 48: GM49; HMIGM49; b45M49; f34GM49; String Ensemble2; am049; am049.in

    // Amplitude begins at 2323.6, peaks 4179.7 at 0.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.8s.
    {  49, 49,  0,  40000,   773 }, // 49: GM50; HMIGM50; b45M50; f34GM50; hamM20; intM20; rickM20; PMellow; PMellow.; Synth Strings 1; am050; am050.in

    // Amplitude begins at    0.0, peaks 1001.6 at 0.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.3s.
    {  50, 50,  0,  40000,   280 }, // 50: GM51; HMIGM51; b45M51; f34GM51; f48GM51; sGM51; SynthStrings 2; am051; am051.in

    // Amplitude begins at  889.7, peaks 3770.3 at 20.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    {  51, 51,  0,  40000,   133 }, // 51: GM52; HMIGM52; b45M52; f34GM52; rickM85; Choir Aahs; Choir.in; am052; am052.in

    // Amplitude begins at    6.6, peaks 3005.9 at 0.2s,
    // fades to 20% at 4.3s, keyoff fades to 20% in 4.3s.
    {  52, 52,  0,   4346,  4346 }, // 52: GM53; HMIGM53; b45M53; f34GM53; rickM86; sGM53; Oohs.ins; Voice Oohs; am053; am053.in

    // Amplitude begins at   48.8, peaks 3994.2 at 19.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    {  53, 53,  0,  40000,   106 }, // 53: GM54; HMIGM54; b45M54; f34GM54; sGM54; Synth Voice; am054; am054.in

    // Amplitude begins at  899.0, peaks 1546.5 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    {  54, 54,  0,    260,   260 }, // 54: GM55; HMIGM55; b45M55; f34GM55; Orchestra Hit; am055; am055.in

    // Amplitude begins at   39.3, peaks  930.6 at 39.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    {  55, 55,  0,  40000,    20 }, // 55: GM56; HMIGM56; b45M56; f17GM56; f34GM56; mGM56; Trumpet; am056; am056.in

    // Amplitude begins at   40.6, peaks  925.9 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    {  56, 56,  0,  40000,    20 }, // 56: GM57; HMIGM57; b45M57; f17GM57; f29GM90; f30GM90; f34GM57; mGM57; Pad 3 polysynth; Trombone; am057; am057.in

    // Amplitude begins at   39.5, peaks 1061.2 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    {  57, 57,  0,  40000,    26 }, // 57: GM58; HMIGM58; b45M58; f34GM58; Tuba; am058; am058.in

    // Amplitude begins at  886.2, peaks 2598.5 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    {  58, 58,  0,  40000,    20 }, // 58: GM59; HMIGM59; b45M59; f17GM59; f34GM59; f35GM59; mGM59; sGM59; Muted Trumpet; am059; am059.in

    // Amplitude begins at    6.8, peaks 4177.4 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    {  59, 59,  0,  40000,     6 }, // 59: GM60; HMIGM60; b45M60; f17GM60; f29GM92; f29GM93; f30GM92; f30GM93; f34GM60; f48GM62; mGM60; French Horn; Pad 5 bowedpad; Pad 6 metallic; Synth Brass 1; am060; am060.in

    // Amplitude begins at    4.0, peaks 1724.3 at 21.7s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    {  60, 60,  0,  40000,     0 }, // 60: GM61; HMIGM61; b45M61; f34GM61; Brass Section; am061; am061.in

    // Amplitude begins at    7.2, peaks 2980.9 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    {  61, 61,  0,  40000,    20 }, // 61: GM62; b45M62; f34GM62; sGM62; Synth Brass 1; am062

    // Amplitude begins at  894.6, peaks 4216.8 at 0.1s,
    // fades to 20% at 0.7s, keyoff fades to 20% in 0.7s.
    {  62, 62,  0,    700,   700 }, // 62: GM63; HMIGM63; b45M63; f17GM63; f29GM26; f29GM44; f30GM26; f30GM44; f34GM63; mGM63; sGM63; Electric Guitar1; Synth Brass 2; Tremulo Strings; am063; am063.in

    // Amplitude begins at  674.0, peaks 3322.1 at 1.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    {  63, 63,  0,  40000,   126 }, // 63: GM64; HMIGM64; b45M64; f34GM64; sGM64; Soprano Sax; am064; am064.in

    // Amplitude begins at    3.5, peaks 1727.6 at 14.7s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    {  64, 64,  0,  40000,    20 }, // 64: GM65; HMIGM65; b45M65; f34GM65; sGM65; Alto Sax; am065; am065.in

    // Amplitude begins at  979.4, peaks 3008.4 at 27.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    {  65, 65,  0,  40000,   106 }, // 65: GM66; HMIGM66; b45M66; f34GM66; sGM66; Tenor Sax; am066; am066.in

    // Amplitude begins at    3.0, peaks 1452.8 at 14.7s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    {  66, 66,  0,  40000,     0 }, // 66: GM67; HMIGM67; b45M67; f34GM67; sGM67; Baritone Sax; am067; am067.in

    // Amplitude begins at  486.9, peaks 2127.8 at 39.5s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    {  67, 67,  0,  40000,    66 }, // 67: GM68; HMIGM68; b45M68; f17GM68; f29GM84; f30GM84; f34GM68; mGM68; Lead 5 charang; Oboe; am068; am068.in

    // Amplitude begins at    5.0, peaks  784.7 at 34.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    {  68, 68,  0,  40000,     6 }, // 68: GM69; HMIGM69; b45M69; f17GM69; f29GM85; f30GM85; f34GM69; f35GM69; mGM69; sGM69; English Horn; Lead 6 voice; am069; am069.in

    // Amplitude begins at   10.1, peaks 3370.6 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    {  69, 69,  0,  40000,     6 }, // 69: GM70; HMIGM70; b45M70; f17GM70; f29GM86; f30GM86; f34GM70; mGM70; Bassoon; Lead 7 fifths; am070; am070.in

    // Amplitude begins at    4.5, peaks 1934.6 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    {  70, 70,  0,  40000,    60 }, // 70: GM71; HMIGM71; b45M71; f17GM71; f29GM82; f29GM83; f30GM82; f30GM83; f34GM71; mGM71; Clarinet; Lead 3 calliope; Lead 4 chiff; am071; am071.in

    // Amplitude begins at   64.0, peaks 4162.8 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    {  71, 71,  0,  40000,     6 }, // 71: GM72; HMIGM72; b45M72; f17GM72; f34GM72; f35GM72; mGM72; Piccolo; am072; am072.in

    // Amplitude begins at    0.5, peaks 2752.3 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    {  72, 72,  0,  40000,     6 }, // 72: GM73; HMIGM73; b45M73; f17GM73; f29GM72; f29GM73; f30GM72; f30GM73; f34GM73; mGM73; Flute; Piccolo; am073; am073.in

    // Amplitude begins at    5.6, peaks 2782.3 at 34.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    {  73, 73,  0,  40000,     6 }, // 73: GM74; HMIGM74; b45M74; sGM74; Recorder; am074; am074.in

    // Amplitude begins at  360.9, peaks 4296.6 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    {  74, 74,  0,  40000,     6 }, // 74: GM75; HMIGM75; b45M75; f17GM75; f29GM77; f30GM77; f34GM75; f35GM75; mGM75; sGM75; Pan Flute; Shakuhachi; am075; am075.in

    // Amplitude begins at    0.8, peaks 2730.2 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    {  75, 75,  0,  40000,    40 }, // 75: GM76; HMIGM76; b45M76; f34GM76; sGM76; Bottle Blow; am076; am076.in

    // Amplitude begins at    7.6, peaks 3112.2 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    {  76, 76,  0,  40000,    46 }, // 76: GM77; HMIGM77; b45M77; f34GM77; sGM77; Shakuhachi; am077; am077.in

    // Amplitude begins at    0.0, peaks 2920.2 at 32.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    {  77, 77,  0,  40000,    73 }, // 77: GM78; HMIGM78; b45M78; f34GM78; sGM78; Whistle; am078; am078.in

    // Amplitude begins at    5.0, peaks 3460.4 at 13.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    {  78, 78,  0,  40000,    66 }, // 78: GM79; HMIGM79; b45M79; f34GM79; hamM61; sGM79; Ocarina; am079; am079.in; ocarina

    // Amplitude begins at 2183.6, peaks 2909.4 at 29.5s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.5s.
    {  79, 79,  0,  40000,   453 }, // 79: GM80; HMIGM80; b45M80; f17GM80; f29GM47; f30GM47; f34GM80; f35GM80; f47GM80; hamM16; hamM65; intM16; mGM80; rickM16; sGM80; LSquare; LSquare.; Lead 1 squareea; Timpany; am080; am080.in; squarewv

    // Amplitude begins at 1288.7, peaks 1362.9 at 35.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    {  80, 80,  0,  40000,     6 }, // 80: GM81; HMIGM81; b45M81; f17GM81; f34GM81; mGM81; sGM81; Lead 2 sawtooth; am081; am081.in

    // Amplitude begins at    0.4, peaks 3053.0 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    {  81, 81,  0,  40000,    26 }, // 81: GM82; HMIGM82; b45M82; f17GM82; f34GM82; mGM82; sGM82; Lead 3 calliope; am082; am082.in

    // Amplitude begins at  867.8, peaks 5910.5 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.3s.
    {  82, 82,  0,  40000,   300 }, // 82: GM83; HMIGM83; b45M83; f34GM83; sGM83; Lead 4 chiff; am083; am083.in

    // Amplitude begins at  991.1, peaks 3848.2 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    {  83, 83,  0,  40000,     6 }, // 83: GM84; HMIGM84; b45M84; f17GM84; f34GM84; mGM84; sGM84; Lead 5 charang; am084; am084.in

    // Amplitude begins at    0.5, peaks 2501.6 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    {  84, 84,  0,  40000,    60 }, // 84: GM85; HMIGM85; b45M85; f34GM85; hamM17; intM17; rickM17; rickM87; sGM85; Lead 6 voice; PFlutes; PFlutes.; Solovox.; am085; am085.in

    // Amplitude begins at  113.3, peaks 1090.0 at 29.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    {  85, 85,  0,  40000,   126 }, // 85: GM86; HMIGM86; b45M86; f34GM86; rickM93; sGM86; Lead 7 fifths; Saw_wave; am086; am086.in

    // Amplitude begins at 2582.4, peaks 3331.5 at 0.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    {  86, 86,  0,  40000,    13 }, // 86: GM87; HMIGM87; b45M87; f17GM87; f34GM87; f35GM87; mGM87; sGM87; Lead 8 brass; am087; am087.in

    // Amplitude begins at 1504.8, peaks 3734.7 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    {  87, 87,  0,  40000,   160 }, // 87: GM88; HMIGM88; b45M88; f34GM88; sGM88; Pad 1 new age; am088; am088.in

    // Amplitude begins at 1679.1, peaks 3279.1 at 24.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 2.9s.
    {  88, 88,  0,  40000,  2880 }, // 88: GM89; HMIGM89; b45M89; f34GM89; sGM89; Pad 2 warm; am089; am089.in

    // Amplitude begins at  641.8, peaks 4073.9 at 0.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    {  89, 89,  0,  40000,   106 }, // 89: GM90; HMIGM90; b45M90; f34GM90; hamM21; intM21; rickM21; sGM90; LTriang; LTriang.; Pad 3 polysynth; am090; am090.in

    // Amplitude begins at    7.2, peaks 4761.3 at 6.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.5s.
    {  90, 90,  0,  40000,  1526 }, // 90: GM91; HMIGM91; b45M91; f34GM91; rickM95; sGM91; Pad 4 choir; Spacevo.; am091; am091.in

    // Amplitude begins at    0.0, peaks 3767.6 at 1.2s,
    // fades to 20% at 4.6s, keyoff fades to 20% in 0.0s.
    {  91, 91,  0,   4586,     6 }, // 91: GM92; HMIGM92; b45M92; f34GM92; f47GM92; sGM92; Pad 5 bowedpad; am092; am092.in

    // Amplitude begins at    0.0, peaks 1692.3 at 0.6s,
    // fades to 20% at 2.8s, keyoff fades to 20% in 2.8s.
    {  92, 92,  0,   2773,  2773 }, // 92: GM93; HMIGM93; b45M93; f34GM93; hamM22; intM22; rickM22; sGM93; PSlow; PSlow.in; Pad 6 metallic; am093; am093.in

    // Amplitude begins at    0.0, peaks 3007.4 at 2.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    {  93, 93,  0,  40000,   193 }, // 93: GM94; HMIGM94; b45M94; f34GM94; hamM23; hamM54; intM23; rickM23; rickM96; sGM94; Halopad.; PSweep; PSweep.i; Pad 7 halo; am094; am094.in; halopad

    // Amplitude begins at 2050.9, peaks 4177.7 at 2.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    {  94, 94,  0,  40000,    53 }, // 94: GM95; HMIGM95; b45M95; f34GM95; f47GM95; hamM66; rickM97; Pad 8 sweep; Sweepad.; am095; am095.in; sweepad

    // Amplitude begins at  852.7, peaks 2460.9 at 0.0s,
    // fades to 20% at 1.0s, keyoff fades to 20% in 1.0s.
    {  95, 95,  0,    993,   993 }, // 95: GM96; HMIGM96; b45M96; f34GM96; sGM96; FX 1 rain; am096; am096.in

    // Amplitude begins at    0.0, peaks 4045.6 at 1.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.6s.
    {  96, 96,  0,  40000,   580 }, // 96: GM97; HMIGM97; b45M97; f17GM97; f29GM36; f30GM36; f34GM97; mGM97; sGM97; FX 2 soundtrack; Slap Bass 1; am097; am097.in

    // Amplitude begins at 1790.2, peaks 3699.2 at 0.0s,
    // fades to 20% at 0.5s, keyoff fades to 20% in 0.5s.
    {  97, 97,  0,    540,   540 }, // 97: GM98; HMIGM98; b45M98; f17GM98; f34GM98; f35GM98; mGM98; sGM98; FX 3 crystal; am098; am098.in

    // Amplitude begins at  992.2, peaks 1029.0 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    {  98, 98,  0,  40000,    93 }, // 98: GM99; HMIGM99; b45M99; f34GM99; sGM99; FX 4 atmosphere; am099; am099.in

    // Amplitude begins at 3083.3, peaks 3480.3 at 0.0s,
    // fades to 20% at 2.2s, keyoff fades to 20% in 2.2s.
    {  99, 99,  0,   2233,  2233 }, // 99: GM100; HMIGM100; b45M100; f34GM100; hamM51; sGM100; FX 5 brightness; am100; am100.in

    // Amplitude begins at    0.0, peaks 1686.6 at 2.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.8s.
    { 100,100,  0,  40000,   800 }, // 100: GM101; HMIGM101; b45M101; f34GM101; sGM101; FX 6 goblins; am101; am101.in

    // Amplitude begins at    0.0, peaks 1834.1 at 4.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.8s.
    { 101,101,  0,  40000,   773 }, // 101: GM102; HMIGM102; b45M102; f34GM102; rickM98; sGM102; Echodrp1; FX 7 echoes; am102; am102.in

    // Amplitude begins at   88.5, peaks 2197.7 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.5s.
    { 102,102,  0,  40000,   453 }, // 102: GM103; HMIGM103; b45M103; f34GM103; sGM103; FX 8 sci-fi; am103; am103.in

    // Amplitude begins at 2640.7, peaks 2812.3 at 0.0s,
    // fades to 20% at 2.3s, keyoff fades to 20% in 2.3s.
    { 103,103,  0,   2306,  2306 }, // 103: GM104; HMIGM104; b45M104; f17GM104; f29GM63; f30GM63; f34GM104; mGM104; sGM104; Sitar; Synth Brass 2; am104; am104.in

    // Amplitude begins at 2465.5, peaks 2912.3 at 0.0s,
    // fades to 20% at 1.1s, keyoff fades to 20% in 1.1s.
    { 104,104,  0,   1053,  1053 }, // 104: GM105; HMIGM105; b45M105; f17GM105; f34GM105; mGM105; sGM105; Banjo; am105; am105.in

    // Amplitude begins at  933.2,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    { 105,105,  0,   1160,  1160 }, // 105: GM106; HMIGM106; b45M106; f17GM106; f34GM106; hamM24; intM24; mGM106; rickM24; sGM106; LDist; LDist.in; Shamisen; am106; am106.in

    // Amplitude begins at 2865.6,
    // fades to 20% at 0.7s, keyoff fades to 20% in 0.7s.
    { 106,106,  0,    653,   653 }, // 106: GM107; HMIGM107; b45M107; f34GM107; sGM107; Koto; am107; am107.in

    // Amplitude begins at 2080.0,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 107,107,  0,    153,   153 }, // 107: GM108; HMIGM108; b45M108; f17GM108; f34GM108; f35GM108; hamM57; mGM108; sGM108; Kalimba; am108; am108.in; kalimba

    // Amplitude begins at    6.6, peaks 2652.1 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 108,108,  0,  40000,     0 }, // 108: GM109; HMIGM109; b45M109; f17GM109; f34GM109; f35GM109; f49GM109; mGM109; sGM109; Bagpipe; am109; am109.in

    // Amplitude begins at  533.2, peaks 1795.4 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 109,109,  0,  40000,   106 }, // 109: GM110; HMIGM110; b45M110; f17GM110; f34GM110; f35GM110; mGM110; sGM110; Fiddle; am110; am110.in

    // Amplitude begins at   66.0, peaks 1441.9 at 16.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 110,110,  0,  40000,     6 }, // 110: GM111; HMIGM111; b45M111; f17GM111; f34GM111; f35GM111; mGM111; sGM111; Shanai; am111; am111.in

    // Amplitude begins at 1669.3, peaks 1691.7 at 0.0s,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    { 111,111,  0,   1226,  1226 }, // 111: GM112; HMIGM112; b45M112; f17GM112; f34GM112; mGM112; sGM112; Tinkle Bell; am112; am112.in

    // Amplitude begins at 1905.2,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 112,112,  0,    146,   146 }, // 112: GM113; HMIGM113; b45M113; f17GM113; f34GM113; f35GM113; hamM50; mGM113; sGM113; Agogo Bells; agogo; am113; am113.in

    // Amplitude begins at 1008.1, peaks 3001.4 at 0.1s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 113,113,  0,    226,   226 }, // 113: GM114; HMIGM114; b45M114; f17GM114; f34GM114; f35GM114; mGM114; sGM114; Steel Drums; am114; am114.in

    // Amplitude begins at  894.1,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 114,114,  0,     26,    26 }, // 114: GM115; HMIGM115; b45M115; f17GM115; f34GM115; f35GM115; mGM115; rickM100; sGM115; Woodblk.; Woodblock; am115; am115.in

    // Amplitude begins at 1571.0, peaks 1764.0 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 115,115,  0,    146,   146 }, // 115: GM116; HMIGM116; b45M116; f17GM116; f29GM118; f30GM117; f30GM118; f34GM116; f35GM116; hamM69; mGM116; Melodic Tom; Synth Drum; Taiko; Taiko Drum; am116; am116.in

    // Amplitude begins at 1088.6, peaks 1805.2 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 116,116,  0,     40,    40 }, // 116: GM117; HMIGM117; b45M117; f17GM117; f29GM113; f30GM113; f34GM117; f35GM117; hamM58; mGM117; sGM117; Agogo Bells; Melodic Tom; am117; am117.in; melotom

    // Amplitude begins at  781.7, peaks  845.9 at 0.0s,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 117,117,  0,    386,   386 }, // 117: GM118; HMIGM118; b45M118; f17GM118; f34GM118; mGM118; Synth Drum; am118; am118.in

    // Amplitude begins at    0.0, peaks  452.7 at 2.2s,
    // fades to 20% at 2.3s, keyoff fades to 20% in 2.3s.
    { 118,118,  0,   2333,  2333 }, // 118: GM119; HMIGM119; b45M119; f34GM119; mGM119; Reverse Cymbal; am119; am119.in

    // Amplitude begins at    0.0, peaks  363.9 at 0.1s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 119,119,  0,    313,   313 }, // 119: GM120; HMIGM120; b45M120; f17GM120; f34GM120; f35GM120; hamM36; intM36; mGM120; rickM101; rickM36; sGM120; DNoise1; DNoise1.; Fretnos.; Guitar FretNoise; am120; am120.in

    // Amplitude begins at    0.0, peaks  472.1 at 0.3s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 120,120,  0,    586,   586 }, // 120: GM121; HMIGM121; b45M121; f17GM121; f34GM121; f35GM121; mGM121; sGM121; Breath Noise; am121; am121.in

    // Amplitude begins at    0.0, peaks  449.3 at 2.3s,
    // fades to 20% at 4.4s, keyoff fades to 20% in 4.4s.
    { 121,121,  0,   4380,  4380 }, // 121: GM122; HMIGM122; b45M122; f17GM122; f34GM122; mGM122; sGM122; Seashore; am122; am122.in

    // Amplitude begins at    0.6, peaks 2634.5 at 0.1s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 122,122,  0,    320,   320 }, // 122: GM123; HMIGM123; b45M123; f15GM124; f17GM123; f26GM124; f29GM124; f30GM124; f34GM123; mGM123; sGM123; Bird Tweet; Telephone; am123; am123.in

    // Amplitude begins at 1196.7,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 123,123,  0,    186,   186 }, // 123: GM124; HMIGM124; b45M124; f17GM124; f29GM123; f30GM123; f34GM124; mGM124; sGM124; Bird Tweet; Telephone; am124; am124.in

    // Amplitude begins at    0.0, peaks  389.6 at 0.1s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 124,124,  0,    146,   146 }, // 124: GM125; HMIGM125; b45M125; f17GM125; f34GM125; mGM125; sGM125; Helicopter; am125; am125.in

    // Amplitude begins at    0.0, peaks  459.9 at 2.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 125,125,  0,  40000,   113 }, // 125: GM126; HMIGM126; b45M126; f17GM126; f34GM126; f35GM126; mGM126; sGM126; Applause/Noise; am126; am126.in

    // Amplitude begins at  361.9,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 126,126,  0,    160,   160 }, // 126: GM127; HMIGM127; b45M127; f17GM127; f34GM127; mGM127; sGM127; Gunshot; am127; am127.in

    // Amplitude begins at 1410.4,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 127,127, 16,     40,    40 }, // 127: GP35; GP36; f17GP35; f17GP36; f20GP35; f20GP36; f29GP35; f29GP36; f30GP35; f30GP36; f31GP31; f31GP35; f31GP36; f34GP35; f34GP36; f35GP35; f42GP36; mGP35; mGP36; qGP35; qGP36; Ac Bass Drum; Bass Drum 1

    // Amplitude begins at  879.9,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 128,128,  2,     26,    26 }, // 128: GP37; f17GP37; f23GP54; f29GP37; f30GP37; f34GP37; f49GP37; mGP37; Side Stick; Tambourine

    // Amplitude begins at  574.5,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 129,129,  0,     86,    86 }, // 129: GP38; GP40; f17GP38; f17GP40; f29GP38; f29GP40; f30GP38; f30GP40; f34GP38; f34GP40; f49GP38; mGP38; mGP40; Acoustic Snare; Electric Snare

    // Amplitude begins at 1832.5,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 130,130,  0,     40,    40 }, // 130: GP39; f17GP39; f29GP39; f30GP39; f34GP39; f49GP39; mGP39; Hand Clap

    // Amplitude begins at 1953.1,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 131,131,  0,     40,    40 }, // 131: GP41; GP43; GP45; GP47; GP48; GP50; GP87; f17GP41; f17GP43; f17GP45; f17GP47; f17GP48; f17GP50; f17GP87; f29GP41; f29GP43; f29GP45; f29GP47; f29GP48; f29GP50; f29GP87; f30GP41; f30GP43; f30GP45; f30GP47; f30GP48; f30GP50; f30GP87; f34GP41; f34GP43; f34GP45; f34GP47; f34GP48; f34GP50; f34GP87; f35GP41; f35GP43; f35GP45; f35GP47; f35GP48; f35GP50; f35GP87; f42GP41; f42GP43; f42GP45; f42GP47; f42GP48; f42GP50; f49GP41; f49GP43; f49GP45; f49GP47; f49GP48; f49GP50; f49GP87; mGP41; mGP43; mGP45; mGP47; mGP48; mGP50; mGP87; sGP87; High Floor Tom; High Tom; High-Mid Tom; Low Floor Tom; Low Tom; Low-Mid Tom; Open Surdu

    // Amplitude begins at  376.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 132,132, 12,     33,    33 }, // 132: GP42; f17GP42; f23GP68; f23GP70; f29GP42; f30GP42; f34GP1; f34GP42; mGP42; Closed High Hat; Low Agogo; Maracas

    // Amplitude begins at    1.1, peaks  526.8 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 133,133, 12,     60,    60 }, // 133: GP44; f17GP44; f29GP44; f30GP44; f34GP44; f35GP44; f49GP44; mGP44; Pedal High Hat

    // Amplitude begins at  398.8,
    // fades to 20% at 0.5s, keyoff fades to 20% in 0.5s.
    { 134,134, 12,    486,   486 }, // 134: GP46; f17GP46; f29GP46; f30GP46; f34GP46; f49GP46; mGP46; Open High Hat

    // Amplitude begins at  133.6, peaks  391.6 at 0.0s,
    // fades to 20% at 2.8s, keyoff fades to 20% in 2.8s.
    { 135,135, 14,   2780,  2780 }, // 135: GP49; GP57; f15GP49; f17GP49; f17GP57; f26GP49; f29GP49; f29GP57; f30GP49; f30GP57; f34GP49; f34GP57; f35GP49; f49GP49; f49GP57; mGP49; mGP57; oGP49; Crash Cymbal 1; Crash Cymbal 2

    // Amplitude begins at  480.6,
    // fades to 20% at 0.7s, keyoff fades to 20% in 0.7s.
    { 136,136, 14,    686,   686 }, // 136: GP51; GP59; f17GP51; f17GP59; f29GP51; f29GP59; f30GP51; f30GP59; f34GP51; f34GP59; f35GP51; f35GP59; f49GP51; f49GP59; mGP51; mGP59; sGP51; sGP59; Ride Cymbal 1; Ride Cymbal 2

    // Amplitude begins at  152.3, peaks  719.7 at 0.1s,
    // fades to 20% at 2.6s, keyoff fades to 20% in 2.6s.
    { 137,137, 14,   2566,  2566 }, // 137: GP52; f17GP52; f29GP52; f30GP52; f34GP52; f35GP52; f49GP52; mGP52; Chinese Cymbal

    // Amplitude begins at  913.7, peaks  929.0 at 0.0s,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 138,138, 14,    353,   353 }, // 138: GP53; f17GP53; f29GP53; f30GP53; f34GP53; f35GP53; f49GP53; mGP53; sGP53; Ride Bell

    // Amplitude begins at  598.6, peaks  657.2 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 139,139,  2,     53,    53 }, // 139: GP54; f17GP54; f30GP54; f34GP54; f49GP54; mGP54; Tambourine

    // Amplitude begins at  375.0, peaks  380.3 at 0.0s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 140,140, 78,    593,   593 }, // 140: GP55; f34GP55; Splash Cymbal

    // Amplitude begins at  675.7,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 141,141, 17,     20,    20 }, // 141: GP56; f17GP56; f29GP56; f30GP56; f34GP56; f48GP56; f49GP56; mGP56; sGP56; Cow Bell

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 142,142,128,      0,     0 }, // 142: GP58; f34GP58; Vibraslap

    // Amplitude begins at  494.1,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 143,143,  6,      6,     6 }, // 143: GP60; f17GP60; f29GP60; f30GP60; f34GP60; f48GP60; f49GP60; mGP60; sGP60; High Bongo

    // Amplitude begins at 1333.1,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 144,144,  1,      6,     6 }, // 144: GP61; f15GP61; f17GP61; f26GP61; f29GP61; f30GP61; f34GP61; f48GP61; f49GP61; mGP61; oGP61; sGP61; Low Bongo

    // Amplitude begins at  272.6,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 145,145,  1,     20,    20 }, // 145: GP62; f17GP62; f29GP62; f30GP62; f34GP62; f48GP62; f49GP62; mGP62; sGP62; Mute High Conga

    // Amplitude begins at 1581.2,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 146,146,  1,      6,     6 }, // 146: GP63; f17GP63; f29GP63; f30GP63; f34GP63; f48GP63; f49GP63; mGP63; sGP63; Open High Conga

    // Amplitude begins at  852.5,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 147,147,  1,      6,     6 }, // 147: GP64; f17GP64; f29GP64; f30GP64; f34GP64; f48GP64; f49GP64; mGP64; sGP64; Low Conga

    // Amplitude begins at  694.7,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 148,148,  1,     20,    20 }, // 148: GP65; f17GP65; f29GP65; f30GP65; f34GP65; f35GP65; f35GP66; f48GP65; f49GP65; mGP65; sGP65; High Timbale; Low Timbale

    // Amplitude begins at  840.3,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 149,149,  0,     40,    40 }, // 149: GP66; f17GP66; f30GP66; f34GP66; f48GP66; f49GP66; mGP66; sGP66; Low Timbale

    // Amplitude begins at  776.7,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 150,150,  3,    186,   186 }, // 150: GP67; f17GP67; f29GP67; f30GP67; f34GP67; f35GP67; f35GP68; f49GP67; mGP67; sGP67; High Agogo; Low Agogo

    // Amplitude begins at  860.2, peaks 1114.3 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 151,151,  3,    126,   126 }, // 151: GP68; f17GP68; f29GP68; f30GP68; f34GP68; f49GP68; mGP68; sGP68; Low Agogo

    // Amplitude begins at    0.2, peaks  374.5 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 152,152, 14,    106,   106 }, // 152: GP69; f15GP69; f17GP69; f26GP69; f29GP69; f30GP69; f34GP69; f42GP69; f49GP69; mGP69; Cabasa

    // Amplitude begins at  134.4, peaks  332.5 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 153,153, 14,     20,    20 }, // 153: GP70; f15GP70; f17GP70; f26GP70; f29GP70; f30GP70; f34GP70; f35GP70; f49GP70; mGP70; sGP70; Maracas

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 154,154,215,      0,     0 }, // 154: GP71; f15GP71; f17GP71; f26GP71; f29GP71; f30GP71; f34GP71; f35GP71; f48GP71; f49GP71; mGP71; sGP71; Short Whistle

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 155,155,215,      0,     0 }, // 155: GP72; f15GP72; f17GP72; f26GP72; f29GP72; f30GP72; f34GP72; f35GP72; f48GP72; f49GP72; mGP72; sGP72; Long Whistle

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 156,156,128,      0,     0 }, // 156: GP73; f34GP73; sGP73; Short Guiro

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 157,157,128,      0,     0 }, // 157: GP74; f34GP74; sGP74; Long Guiro

    // Amplitude begins at  959.9, peaks 1702.6 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 158,158,  6,     53,    53 }, // 158: GP75; f15GP75; f17GP75; f26GP75; f29GP75; f30GP75; f34GP75; f49GP75; mGP75; oGP75; sGP75; Claves

    // Amplitude begins at  887.8,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 159,159,  6,      6,     6 }, // 159: GP76; f17GP76; f29GP76; f30GP76; f34GP76; f35GP76; f48GP76; f49GP76; mGP76; High Wood Block

    // Amplitude begins at  814.1,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 160,160,  6,      6,     6 }, // 160: GP77; f17GP77; f29GP77; f30GP77; f34GP77; f35GP77; f48GP77; f49GP77; mGP77; sGP77; Low Wood Block

    // Amplitude begins at    2.0, peaks 1722.0 at 0.1s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 161,161,  1,     93,    93 }, // 161: GP78; f17GP78; f29GP78; f30GP78; f34GP78; f35GP78; f49GP78; mGP78; sGP78; Mute Cuica

    // Amplitude begins at 1277.6, peaks 2872.7 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 162,162, 65,    346,   346 }, // 162: GP79; f34GP79; sGP79; Open Cuica

    // Amplitude begins at 1146.2,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 163,163, 10,    386,   386 }, // 163: GP80; f17GP80; f29GP80; f30GP80; f34GP80; f35GP80; f49GP80; mGP80; sGP80; Mute Triangle

    // Amplitude begins at 1179.8, peaks 1188.2 at 0.0s,
    // fades to 20% at 3.0s, keyoff fades to 20% in 3.0s.
    { 164,164, 10,   3046,  3046 }, // 164: GP81; f17GP81; f29GP81; f30GP81; f34GP81; f35GP81; f49GP81; mGP81; sGP81; Open Triangle

    // Amplitude begins at    0.2, peaks  373.3 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 165,165, 14,     53,    53 }, // 165: GP82; f17GP82; f29GP82; f30GP82; f34GP82; f35GP82; f42GP82; f49GP82; mGP82; sGP82; Shaker

    // Amplitude begins at    0.0, peaks  959.9 at 0.2s,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 166,166, 14,    440,   440 }, // 166: GP83; f17GP83; f29GP83; f30GP83; f34GP83; f35GP83; f48GP83; f49GP83; mGP83; sGP83; Jingle Bell

    // Amplitude begins at 1388.2,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 167,167,  5,      6,     6 }, // 167: GP84; f15GP51; f17GP84; f26GP51; f29GP84; f30GP84; f34GP84; f35GP84; f49GP84; mGP84; sGP84; Bell Tree; Ride Cymbal 1

    // Amplitude begins at  566.5, peaks  860.2 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 168,168,  2,     46,    46 }, // 168: GP85; f17GP85; f29GP85; f30GP85; f34GP85; f35GP85; f48GP85; f49GP85; mGP85; sGP85; Castanets

    // Amplitude begins at 1845.5,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 169,169,  1,     13,    13 }, // 169: GP86; f15GP63; f15GP64; f17GP86; f26GP63; f26GP64; f29GP86; f30GP86; f34GP86; f35GP86; f49GP86; mGP86; sGP86; Low Conga; Mute Surdu; Open High Conga

    // Amplitude begins at   56.3, peaks 1484.4 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 170,170,  0,  40000,    13 }, // 170: HMIGM0; HMIGM29; f17GM29; f35GM29; mGM29; Overdrive Guitar; am029.in

    // Amplitude begins at 3912.6,
    // fades to 20% at 1.3s, keyoff fades to 20% in 0.0s.
    { 171,171,  0,   1326,     6 }, // 171: HMIGM1; f17GM1; mGM1; BrightAcouGrand; am001.in

    // Amplitude begins at 2850.7, peaks 4216.6 at 0.0s,
    // fades to 20% at 1.3s, keyoff fades to 20% in 1.3s.
    { 172,172,  0,   1293,  1293 }, // 172: HMIGM2; f17GM2; f35GM2; mGM2; ElecGrandPiano; am002.in

    // Amplitude begins at 1712.7, peaks 2047.5 at 0.5s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 173,173,  0,  40000,     6 }, // 173: HMIGM3; am003.in

    // Amplitude begins at 4461.0, peaks 6341.0 at 0.0s,
    // fades to 20% at 3.2s, keyoff fades to 20% in 0.0s.
    { 174,174,  0,   3200,     6 }, // 174: HMIGM4; f17GM4; f35GM4; mGM4; Rhodes Piano; am004.in

    // Amplitude begins at 4781.0, peaks 6329.2 at 0.0s,
    // fades to 20% at 3.2s, keyoff fades to 20% in 0.0s.
    { 175,175,  0,   3200,     6 }, // 175: HMIGM5; f17GM5; f35GM5; mGM5; Chorused Piano; am005.in

    // Amplitude begins at 1162.2, peaks 1404.5 at 0.0s,
    // fades to 20% at 2.2s, keyoff fades to 20% in 2.2s.
    { 176,176,  0,   2173,  2173 }, // 176: HMIGM6; f17GM6; mGM6; Harpsichord; am006.in

    // Amplitude begins at 1144.6, peaks 1235.5 at 0.0s,
    // fades to 20% at 2.3s, keyoff fades to 20% in 2.3s.
    { 177,177,  0,   2313,  2313 }, // 177: HMIGM7; f17GM7; mGM7; Clavinet; am007.in

    // Amplitude begins at 2803.9, peaks 2829.0 at 0.0s,
    // fades to 20% at 1.0s, keyoff fades to 20% in 1.0s.
    { 178,178,  0,    960,   960 }, // 178: HMIGM8; am008.in

    // Amplitude begins at 2999.3, peaks 3227.0 at 0.0s,
    // fades to 20% at 1.4s, keyoff fades to 20% in 1.4s.
    { 179,179,  0,   1380,  1380 }, // 179: HMIGM9; f17GM9; mGM9; Glockenspiel; am009.in

    // Amplitude begins at 2073.6, peaks 3450.4 at 0.1s,
    // fades to 20% at 0.5s, keyoff fades to 20% in 0.5s.
    { 180,180,  0,    453,   453 }, // 180: HMIGM10; f17GM10; f35GM10; f48GM10; mGM10; Music box; am010.in

    // Amplitude begins at 2976.7, peaks 3033.0 at 0.0s,
    // fades to 20% at 1.7s, keyoff fades to 20% in 1.7s.
    { 181,181,  0,   1746,  1746 }, // 181: HMIGM11; am011.in

    // Amplitude begins at 3343.0, peaks 3632.8 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 182,182,  0,    206,   206 }, // 182: HMIGM12; f17GM12; mGM12; Marimba; am012.in

    // Amplitude begins at 2959.7, peaks 3202.7 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 183,183,  0,    100,   100 }, // 183: HMIGM13; f17GM13; f35GM13; mGM13; Xylophone; am013.in

    // Amplitude begins at 2057.2, peaks 2301.4 at 0.0s,
    // fades to 20% at 1.3s, keyoff fades to 20% in 1.3s.
    { 184,184,  0,   1306,  1306 }, // 184: HMIGM14; am014.in

    // Amplitude begins at 1673.4, peaks 2155.0 at 0.0s,
    // fades to 20% at 0.5s, keyoff fades to 20% in 0.5s.
    { 185,185,  0,    460,   460 }, // 185: HMIGM15; f48GM15; Dulcimer; am015.in

    // Amplitude begins at 2090.6,
    // fades to 20% at 1.3s, keyoff fades to 20% in 1.3s.
    { 186,186,  0,   1266,  1266 }, // 186: HMIGM27; f17GM27; mGM27; Electric Guitar2; am027.in

    // Amplitude begins at 1957.2, peaks 3738.3 at 0.0s,
    // fades to 20% at 2.9s, keyoff fades to 20% in 2.9s.
    { 187,187,  0,   2886,  2886 }, // 187: HMIGM37; f17GM37; mGM37; Slap Bass 2; am037.in

    // Amplitude begins at    7.2, peaks 3168.6 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 188,188,  0,  40000,    20 }, // 188: HMIGM62; am062.in

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 189,189, 60,      0,     0 }, // 189: HMIGP0; HMIGP1; HMIGP10; HMIGP100; HMIGP101; HMIGP102; HMIGP103; HMIGP104; HMIGP105; HMIGP106; HMIGP107; HMIGP108; HMIGP109; HMIGP11; HMIGP110; HMIGP111; HMIGP112; HMIGP113; HMIGP114; HMIGP115; HMIGP116; HMIGP117; HMIGP118; HMIGP119; HMIGP12; HMIGP120; HMIGP121; HMIGP122; HMIGP123; HMIGP124; HMIGP125; HMIGP126; HMIGP127; HMIGP13; HMIGP14; HMIGP15; HMIGP16; HMIGP17; HMIGP18; HMIGP19; HMIGP2; HMIGP20; HMIGP21; HMIGP22; HMIGP23; HMIGP24; HMIGP25; HMIGP26; HMIGP3; HMIGP4; HMIGP5; HMIGP6; HMIGP7; HMIGP8; HMIGP88; HMIGP89; HMIGP9; HMIGP90; HMIGP91; HMIGP92; HMIGP93; HMIGP94; HMIGP95; HMIGP96; HMIGP97; HMIGP98; HMIGP99; b42P0; b42P1; b42P10; b42P100; b42P101; b42P102; b42P103; b42P104; b42P105; b42P106; b42P107; b42P108; b42P109; b42P11; b42P110; b42P111; b42P112; b42P113; b42P114; b42P115; b42P116; b42P117; b42P118; b42P119; b42P12; b42P120; b42P121; b42P122; b42P123; b42P124; b42P125; b42P126; b42P13; b42P14; b42P15; b42P16; b42P17; b42P18; b42P19; b42P2; b42P20; b42P21; b42P22; b42P23; b42P24; b42P25; b42P26; b42P27; b42P3; b42P4; b42P5; b42P6; b42P7; b42P8; b42P88; b42P89; b42P9; b42P90; b42P91; b42P92; b42P93; b42P94; b42P95; b42P96; b42P97; b42P98; b42P99; b43P0; b43P1; b43P10; b43P100; b43P101; b43P102; b43P103; b43P104; b43P105; b43P106; b43P107; b43P108; b43P109; b43P11; b43P110; b43P111; b43P112; b43P113; b43P114; b43P115; b43P116; b43P117; b43P118; b43P119; b43P12; b43P120; b43P121; b43P122; b43P123; b43P124; b43P125; b43P126; b43P127; b43P13; b43P14; b43P15; b43P16; b43P17; b43P18; b43P19; b43P2; b43P20; b43P21; b43P22; b43P23; b43P24; b43P25; b43P26; b43P27; b43P3; b43P4; b43P5; b43P6; b43P7; b43P8; b43P88; b43P89; b43P9; b43P90; b43P91; b43P92; b43P93; b43P94; b43P95; b43P96; b43P97; b43P98; b43P99; b44M0; b44M1; b44M10; b44M100; b44M101; b44M102; b44M103; b44M104; b44M105; b44M106; b44M107; b44M108; b44M109; b44M11; b44M110; b44M111; b44M112; b44M113; b44M114; b44M115; b44M116; b44M117; b44M118; b44M119; b44M12; b44M120; b44M121; b44M122; b44M123; b44M124; b44M125; b44M126; b44M127; b44M13; b44M14; b44M15; b44M16; b44M17; b44M18; b44M19; b44M2; b44M20; b44M21; b44M22; b44M23; b44M24; b44M25; b44M26; b44M3; b44M4; b44M5; b44M6; b44M7; b44M8; b44M88; b44M89; b44M9; b44M90; b44M91; b44M92; b44M93; b44M94; b44M95; b44M96; b44M97; b44M98; b44M99; Blank; Blank.in

    // Amplitude begins at    4.1, peaks  788.6 at 0.0s,
    // fades to 20% at 0.5s, keyoff fades to 20% in 0.5s.
    { 190,190, 73,    473,   473 }, // 190: HMIGP27; b44M27; Wierd1.i

    // Amplitude begins at    4.2, peaks  817.8 at 0.0s,
    // fades to 20% at 0.5s, keyoff fades to 20% in 0.5s.
    { 190,190, 74,    473,   473 }, // 191: HMIGP28; b44M28; Wierd1.i

    // Amplitude begins at    4.9, peaks  841.9 at 0.0s,
    // fades to 20% at 0.5s, keyoff fades to 20% in 0.5s.
    { 190,190, 80,    473,   473 }, // 192: HMIGP29; b44M29; Wierd1.i

    // Amplitude begins at    5.3, peaks  778.3 at 0.0s,
    // fades to 20% at 0.5s, keyoff fades to 20% in 0.5s.
    { 190,190, 84,    473,   473 }, // 193: HMIGP30; b44M30; Wierd1.i

    // Amplitude begins at    9.5, peaks  753.0 at 0.0s,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 190,190, 92,    406,   406 }, // 194: HMIGP31; b44M31; Wierd1.i

    // Amplitude begins at   91.3, peaks  917.0 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 191,191, 81,    280,   280 }, // 195: HMIGP32; b44M32; Wierd2.i

    // Amplitude begins at   89.5, peaks  919.6 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 191,191, 83,    280,   280 }, // 196: HMIGP33; b44M33; Wierd2.i

    // Amplitude begins at  155.5, peaks  913.4 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 191,191, 95,    240,   240 }, // 197: HMIGP34; b44M34; Wierd2.i

    // Amplitude begins at  425.5, peaks  768.5 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 192,192, 83,    273,   273 }, // 198: HMIGP35; b44M35; Wierd3.i

    // Amplitude begins at 2304.1, peaks 2323.3 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 193,193, 35,     33,    33 }, // 199: HMIGP36; b43P36; Kick; Kick.ins

    // Amplitude begins at 2056.5,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 194,194, 36,     20,    20 }, // 200: HMIGP37; HMIGP86; b43P31; b43P37; b43P86; b44M37; b44M86; RimShot; RimShot.; rimshot; rimshot.

    // Amplitude begins at  752.1,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 195,195, 60,     40,    40 }, // 201: HMIGP38; b43P38; b44M38; Snare; Snare.in

    // Amplitude begins at  585.8,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 196,196, 59,     20,    20 }, // 202: HMIGP39; b43P39; b44M39; Clap; Clap.ins

    // Amplitude begins at  768.1,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 195,195, 44,     40,    40 }, // 203: HMIGP40; b43P40; b44M40; Snare; Snare.in

    // Amplitude begins at  646.2, peaks  724.0 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 197,197, 41,    160,   160 }, // 204: HMIGP41; b43P41; b44M41; Toms; Toms.ins

    // Amplitude begins at 2548.3, peaks 2665.5 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 198,198, 97,    200,   200 }, // 205: HMIGP42; HMIGP44; b44M42; b44M44; clshat97

    // Amplitude begins at  700.8, peaks  776.1 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 197,197, 44,    120,   120 }, // 206: HMIGP43; b43P43; b44M43; Toms; Toms.ins

    // Amplitude begins at  667.5,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 197,197, 48,    160,   160 }, // 207: HMIGP45; b43P45; b44M45; Toms; Toms.ins

    // Amplitude begins at   11.0, peaks  766.2 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 199,199, 96,    253,   253 }, // 208: HMIGP46; b44M46; Opnhat96

    // Amplitude begins at  646.7, peaks  707.8 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 197,197, 51,    166,   166 }, // 209: HMIGP47; b43P47; b44M47; Toms; Toms.ins

    // Amplitude begins at  753.9,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 197,197, 54,    153,   153 }, // 210: HMIGP48; b43P48; b44M48; Toms; Toms.ins

    // Amplitude begins at  473.8,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 200,200, 40,    380,   380 }, // 211: HMIGP49; HMIGP52; HMIGP55; HMIGP57; b43P49; b43P52; b43P55; b43P57; b44M49; b44M52; b44M55; b44M57; Crashcym

    // Amplitude begins at  775.6,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 197,197, 57,    106,   106 }, // 212: HMIGP50; b43P50; b44M50; Toms; Toms.ins

    // Amplitude begins at  360.8,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 201,201, 58,    206,   206 }, // 213: HMIGP51; HMIGP53; b43P51; b43P53; b44M51; b44M53; Ridecym; Ridecym.

    // Amplitude begins at 2347.0, peaks 2714.6 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 202,202, 97,    186,   186 }, // 214: HMIGP54; b43P54; b44M54; Tamb; Tamb.ins

    // Amplitude begins at 2920.4,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 203,203, 50,     73,    73 }, // 215: HMIGP56; b43P56; b44M56; Cowbell; Cowbell.

    // Amplitude begins at  556.9,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 204,204, 28,     26,    26 }, // 216: HMIGP58; b42P58; b43P58; b44M58; vibrasla

    // Amplitude begins at  365.3,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 201,201, 60,    186,   186 }, // 217: HMIGP59; b43P59; b44M59; ridecym; ridecym.

    // Amplitude begins at 2172.1, peaks 2678.2 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 205,205, 53,     46,    46 }, // 218: HMIGP60; b43P60; b44M60; mutecong

    // Amplitude begins at 2346.3, peaks 2455.2 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 206,206, 46,     53,    53 }, // 219: HMIGP61; b43P61; b44M61; conga; conga.in

    // Amplitude begins at 2247.7, peaks 2709.8 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 205,205, 57,     46,    46 }, // 220: HMIGP62; b43P62; b44M62; mutecong

    // Amplitude begins at 1529.6,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 207,207, 42,    133,   133 }, // 221: HMIGP63; b43P63; b44M63; loconga; loconga.

    // Amplitude begins at 1495.0,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 207,207, 37,    133,   133 }, // 222: HMIGP64; b43P64; b44M64; loconga; loconga.

    // Amplitude begins at  486.6, peaks  530.9 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 208,208, 41,    186,   186 }, // 223: HMIGP65; b43P65; b44M65; timbale; timbale.

    // Amplitude begins at  508.9,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 208,208, 37,    193,   193 }, // 224: HMIGP66; b43P66; b44M66; timbale; timbale.

    // Amplitude begins at  789.4,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 209,209, 77,     40,    40 }, // 225: HMIGP67; b43P67; agogo; agogo.in

    // Amplitude begins at  735.2,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 209,209, 72,     46,    46 }, // 226: HMIGP68; b43P68; agogo; agogo.in

    // Amplitude begins at    5.1, peaks  818.7 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 210,210, 70,     66,    66 }, // 227: HMIGP69; HMIGP82; b43P69; b43P82; b44M69; b44M82; shaker; shaker.i

    // Amplitude begins at    4.4, peaks  758.9 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 210,210, 90,     66,    66 }, // 228: HMIGP70; b43P70; b44M70; shaker; shaker.i

    // Amplitude begins at  474.0, peaks 1257.8 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 211,211, 39,    180,   180 }, // 229: HMIGP71; b43P71; b44M71; hiwhist; hiwhist.

    // Amplitude begins at  468.8, peaks 1217.2 at 0.0s,
    // fades to 20% at 0.5s, keyoff fades to 20% in 0.5s.
    { 212,212, 36,    513,   513 }, // 230: HMIGP72; b43P72; b44M72; lowhist; lowhist.

    // Amplitude begins at   28.5, peaks  520.5 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 213,213, 46,     46,    46 }, // 231: HMIGP73; b43P73; b44M73; higuiro; higuiro.

    // Amplitude begins at 1096.9, peaks 2623.5 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 214,214, 48,    253,   253 }, // 232: HMIGP74; b43P74; b44M74; loguiro; loguiro.

    // Amplitude begins at 2832.4,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 215,215, 85,     13,    13 }, // 233: HMIGP75; b43P75; b44M75; clavecb; clavecb.

    // Amplitude begins at 2608.5,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 216,216, 66,     60,    60 }, // 234: HMIGP76; b43P76; b44M76; woodblok

    // Amplitude begins at 2613.2,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 216,216, 61,     60,    60 }, // 235: HMIGP77; b43P33; b43P77; b44M77; woodblok

    // Amplitude begins at    2.6, peaks 2733.2 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 217,217, 41,    120,   120 }, // 236: HMIGP78; b43P78; b44M78; hicuica; hicuica.

    // Amplitude begins at    2.4, peaks 2900.3 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 218,218, 41,  40000,    20 }, // 237: HMIGP79; b43P79; b44M79; locuica; locuica.

    // Amplitude begins at 1572.0,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 219,219, 81,     66,    66 }, // 238: HMIGP80; b43P80; b44M80; mutringl

    // Amplitude begins at 1668.0,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 220,220, 81,    260,   260 }, // 239: HMIGP81; b42P81; b43P81; b44M81; triangle

    // Amplitude begins at 1693.0,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 220,220, 76,    260,   260 }, // 240: HMIGP83; b42P83; b43P83; b44M83; triangle

    // Amplitude begins at 1677.5,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 220,220,103,    220,   220 }, // 241: HMIGP84; b43P84; b44M84; triangle

    // Amplitude begins at 1635.5,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 194,194, 60,     13,    13 }, // 242: HMIGP85; b43P85; b44M85; rimShot; rimShot.

    // Amplitude begins at  976.1,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 221,221, 53,    120,   120 }, // 243: HMIGP87; b43P87; b44M87; taiko; taiko.in

    // Amplitude begins at   64.3,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 222,222,  0,      6,     6 }, // 244: hamM0; hamM100; hamM101; hamM102; hamM103; hamM104; hamM105; hamM106; hamM107; hamM108; hamM109; hamM110; hamM111; hamM112; hamM113; hamM114; hamM115; hamM116; hamM117; hamM118; hamM119; hamM126; hamM49; hamM74; hamM75; hamM76; hamM77; hamM78; hamM79; hamM80; hamM81; hamM82; hamM83; hamM84; hamM85; hamM86; hamM87; hamM88; hamM89; hamM90; hamM91; hamM92; hamM93; hamM94; hamM95; hamM96; hamM97; hamM98; hamM99; hamP100; hamP101; hamP102; hamP103; hamP104; hamP105; hamP106; hamP107; hamP108; hamP109; hamP110; hamP111; hamP112; hamP113; hamP114; hamP115; hamP116; hamP117; hamP118; hamP119; hamP120; hamP121; hamP122; hamP123; hamP124; hamP125; hamP126; hamP20; hamP21; hamP22; hamP23; hamP24; hamP25; hamP26; hamP93; hamP94; hamP95; hamP96; hamP97; hamP98; hamP99; intM0; intM100; intM101; intM102; intM103; intM104; intM105; intM106; intM107; intM108; intM109; intM110; intM111; intM112; intM113; intM114; intM115; intM116; intM117; intM118; intM119; intM120; intM121; intM122; intM123; intM124; intM125; intM126; intM127; intM50; intM51; intM52; intM53; intM54; intM55; intM56; intM57; intM58; intM59; intM60; intM61; intM62; intM63; intM64; intM65; intM66; intM67; intM68; intM69; intM70; intM71; intM72; intM73; intM74; intM75; intM76; intM77; intM78; intM79; intM80; intM81; intM82; intM83; intM84; intM85; intM86; intM87; intM88; intM89; intM90; intM91; intM92; intM93; intM94; intM95; intM96; intM97; intM98; intM99; intP0; intP1; intP10; intP100; intP101; intP102; intP103; intP104; intP105; intP106; intP107; intP108; intP109; intP11; intP110; intP111; intP112; intP113; intP114; intP115; intP116; intP117; intP118; intP119; intP12; intP120; intP121; intP122; intP123; intP124; intP125; intP126; intP127; intP13; intP14; intP15; intP16; intP17; intP18; intP19; intP2; intP20; intP21; intP22; intP23; intP24; intP25; intP26; intP3; intP4; intP5; intP6; intP7; intP8; intP9; intP94; intP95; intP96; intP97; intP98; intP99; rickM0; rickM102; rickM103; rickM104; rickM105; rickM106; rickM107; rickM108; rickM109; rickM110; rickM111; rickM112; rickM113; rickM114; rickM115; rickM116; rickM117; rickM118; rickM119; rickM120; rickM121; rickM122; rickM123; rickM124; rickM125; rickM126; rickM127; rickM49; rickM50; rickM51; rickM52; rickM53; rickM54; rickM55; rickM56; rickM57; rickM58; rickM59; rickM60; rickM61; rickM62; rickM63; rickM64; rickM65; rickM66; rickM67; rickM68; rickM69; rickM70; rickM71; rickM72; rickM73; rickM74; rickM75; rickP0; rickP1; rickP10; rickP106; rickP107; rickP108; rickP109; rickP11; rickP110; rickP111; rickP112; rickP113; rickP114; rickP115; rickP116; rickP117; rickP118; rickP119; rickP12; rickP120; rickP121; rickP122; rickP123; rickP124; rickP125; rickP126; rickP127; rickP2; rickP3; rickP4; rickP5; rickP6; rickP7; rickP8; rickP9; nosound; nosound.

    // Amplitude begins at  947.4,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 223,223,  0,     40,    40 }, // 245: hamM1; intM1; rickM1; DBlock; DBlock.i

    // Amplitude begins at 2090.6,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    { 224,224,  0,   1213,  1213 }, // 246: hamM2; intM2; rickM2; GClean; GClean.i

    // Amplitude begins at 1555.8, peaks 1691.9 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 225,225,  0,    126,   126 }, // 247: hamM4; intM4; rickM4; DToms; DToms.in

    // Amplitude begins at  733.0, peaks  761.8 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 226,226,  0,  40000,    20 }, // 248: f53GM63; hamM7; intM7; rickM7; rickM84; GOverD; GOverD.i; Guit_fz2; Synth Brass 2

    // Amplitude begins at 1258.5, peaks 1452.4 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 227,227,  0,  40000,     6 }, // 249: hamM8; intM8; rickM8; GMetal; GMetal.i

    // Amplitude begins at 2589.3, peaks 2634.3 at 0.0s,
    // fades to 20% at 0.7s, keyoff fades to 20% in 0.7s.
    { 228,228,  0,    706,   706 }, // 250: hamM9; intM9; rickM9; BPick; BPick.in

    // Amplitude begins at 1402.0, peaks 2135.8 at 0.0s,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 229,229,  0,    446,   446 }, // 251: hamM10; intM10; rickM10; BSlap; BSlap.in

    // Amplitude begins at 3040.5, peaks 3072.9 at 0.0s,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 230,230,  0,    440,   440 }, // 252: hamM11; intM11; rickM11; BSynth1; BSynth1.

    // Amplitude begins at 2530.9, peaks 2840.4 at 0.0s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 231,231,  0,    626,   626 }, // 253: hamM12; intM12; rickM12; BSynth2; BSynth2.

    // Amplitude begins at   39.5, peaks 1089.7 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 232,232,  0,  40000,    26 }, // 254: hamM15; intM15; rickM15; PSoft; PSoft.in

    // Amplitude begins at    0.0, peaks 1440.0 at 0.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 233,233,  0,  40000,    26 }, // 255: hamM18; intM18; rickM18; PRonStr1

    // Amplitude begins at    0.0, peaks 1417.7 at 0.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 234,234,  0,  40000,    26 }, // 256: hamM19; intM19; rickM19; PRonStr2

    // Amplitude begins at   73.3, peaks 1758.3 at 0.0s,
    // fades to 20% at 1.9s, keyoff fades to 20% in 1.9s.
    { 235,235,  0,   1940,  1940 }, // 257: hamM25; intM25; rickM25; LTrap; LTrap.in

    // Amplitude begins at 2192.9, peaks 3442.1 at 0.0s,
    // fades to 20% at 1.9s, keyoff fades to 20% in 1.9s.
    { 236,236,  0,   1886,  1886 }, // 258: hamM26; intM26; rickM26; LSaw; LSaw.ins

    // Amplitude begins at 1119.0, peaks 1254.3 at 0.1s,
    // fades to 20% at 1.7s, keyoff fades to 20% in 1.7s.
    { 237,237,  0,   1733,  1733 }, // 259: hamM27; intM27; rickM27; PolySyn; PolySyn.

    // Amplitude begins at 2355.1, peaks 3374.2 at 0.5s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 238,238,  0,  40000,     0 }, // 260: hamM28; intM28; rickM28; Pobo; Pobo.ins

    // Amplitude begins at  679.7, peaks 2757.2 at 0.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 239,239,  0,  40000,    20 }, // 261: hamM29; intM29; rickM29; PSweep2; PSweep2.

    // Amplitude begins at 1971.4, peaks 2094.8 at 0.0s,
    // fades to 20% at 1.4s, keyoff fades to 20% in 1.4s.
    { 240,240,  0,   1386,  1386 }, // 262: hamM30; intM30; rickM30; LBright; LBright.

    // Amplitude begins at    0.0, peaks  857.0 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 241,241,  0,  40000,    73 }, // 263: hamM31; intM31; rickM31; SynStrin

    // Amplitude begins at  901.8, peaks  974.7 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 242,242,  0,  40000,   106 }, // 264: hamM32; intM32; rickM32; SynStr2; SynStr2.

    // Amplitude begins at 2098.8,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 243,243,  0,    560,   560 }, // 265: hamM33; intM33; rickM33; low_blub

    // Amplitude begins at  978.8, peaks 2443.9 at 1.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 2.4s.
    { 244,244,  0,  40000,  2446 }, // 266: hamM34; intM34; rickM34; DInsect; DInsect.

    // Amplitude begins at  426.5, peaks  829.0 at 0.1s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 245,245,  0,    140,   140 }, // 267: f25GM0; hamM35; intM35; rickM35; AcouGrandPiano; hardshak

    // Amplitude begins at  982.9, peaks 2185.9 at 0.1s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 246,246,  0,    260,   260 }, // 268: hamM37; intM37; rickM37; WUMP; WUMP.ins

    // Amplitude begins at  557.9,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 247,247,  0,    160,   160 }, // 269: hamM38; intM38; rickM38; DSnare; DSnare.i

    // Amplitude begins at 2110.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 248,248,  0,     20,    20 }, // 270: f25GM112; hamM39; intM39; rickM39; DTimp; DTimp.in; Tinkle Bell

    // Amplitude begins at    0.0, peaks  830.0 at 1.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 249,249,  0,  40000,     0 }, // 271: hamM40; intM40; rickM40; DRevCym; DRevCym.

    // Amplitude begins at    0.0, peaks 1668.5 at 0.2s,
    // fades to 20% at 0.8s, keyoff fades to 20% in 0.8s.
    { 250,250,  0,    780,   780 }, // 272: hamM41; intM41; rickM41; Dorky; Dorky.in

    // Amplitude begins at    4.1, peaks 2699.0 at 0.1s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 251,251,  0,     73,    73 }, // 273: hamM42; intM42; rickM42; DFlab; DFlab.in

    // Amplitude begins at 1995.3, peaks 3400.6 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 252,252,  0,  40000,    60 }, // 274: hamM43; intM43; rickM43; DInsect2

    // Amplitude begins at    0.0, peaks  792.4 at 2.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.4s.
    { 253,253,  0,  40000,   406 }, // 275: hamM44; intM44; rickM44; DChopper

    // Amplitude begins at  787.3, peaks  848.0 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 254,254,  0,    306,   306 }, // 276: hamM45; intM45; rickM45; DShot; DShot.in

    // Amplitude begins at 2940.9, peaks 3003.1 at 0.0s,
    // fades to 20% at 1.8s, keyoff fades to 20% in 1.8s.
    { 255,255,  0,   1780,  1780 }, // 277: hamM46; intM46; rickM46; KickAss; KickAss.

    // Amplitude begins at 1679.1, peaks 1782.4 at 0.1s,
    // fades to 20% at 1.7s, keyoff fades to 20% in 1.7s.
    { 256,256,  0,   1693,  1693 }, // 278: hamM47; intM47; rickM47; RVisCool

    // Amplitude begins at 1214.6, peaks 1237.8 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 257,257,  0,     80,    80 }, // 279: hamM48; intM48; rickM48; DSpring; DSpring.

    // Amplitude begins at    0.0, peaks 3049.5 at 2.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 258,258,  0,  40000,    20 }, // 280: intM49; Chorar22

    // Amplitude begins at 1401.3, peaks 2203.6 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 259,259, 36,     73,    73 }, // 281: hamP27; intP27; rickP27; timpani; timpani.

    // Amplitude begins at 1410.5, peaks 1600.6 at 0.0s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 260,260, 50,    580,   580 }, // 282: hamP28; intP28; rickP28; timpanib

    // Amplitude begins at 1845.9,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 261,261, 37,     33,    33 }, // 283: hamP29; intP29; rickP29; APS043; APS043.i

    // Amplitude begins at 1267.8, peaks 1286.0 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 262,262, 39,    166,   166 }, // 284: hamP30; intP30; rickP30; mgun3; mgun3.in

    // Amplitude begins at 1247.2, peaks 1331.5 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 263,263, 39,     53,    53 }, // 285: hamP31; intP31; rickP31; kick4r; kick4r.i

    // Amplitude begins at 1432.2, peaks 1442.6 at 0.0s,
    // fades to 20% at 1.8s, keyoff fades to 20% in 1.8s.
    { 264,264, 86,   1826,  1826 }, // 286: hamP32; intP32; rickP32; timb1r; timb1r.i

    // Amplitude begins at  996.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 265,265, 43,     33,    33 }, // 287: hamP33; intP33; rickP33; timb2r; timb2r.i

    // Amplitude begins at 1331.2,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 127,127, 24,      6,     6 }, // 288: hamP34; intP34; rickP34; apo035; apo035.i

    // Amplitude begins at 1324.1,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 127,127, 29,     53,    53 }, // 289: hamP35; intP35; rickP35; apo035; apo035.i

    // Amplitude begins at 1604.5,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 266,266, 50,    193,   193 }, // 290: hamP36; intP36; rickP36; hartbeat

    // Amplitude begins at 1443.1, peaks 1537.0 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 267,267, 30,    100,   100 }, // 291: hamP37; intP37; rickP37; tom1r; tom1r.in

    // Amplitude begins at 1351.4, peaks 1552.4 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 267,267, 33,    320,   320 }, // 292: hamP38; intP38; rickP38; tom1r; tom1r.in

    // Amplitude begins at 1362.2, peaks 1455.5 at 0.0s,
    // fades to 20% at 1.6s, keyoff fades to 20% in 1.6s.
    { 267,267, 38,   1646,  1646 }, // 293: hamP39; intP39; rickP39; tom1r; tom1r.in

    // Amplitude begins at 1292.1, peaks 1445.6 at 0.0s,
    // fades to 20% at 1.7s, keyoff fades to 20% in 1.7s.
    { 267,267, 42,   1706,  1706 }, // 294: hamP40; intP40; rickP40; tom1r; tom1r.in

    // Amplitude begins at 2139.1,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 268,268, 24,     46,    46 }, // 295: intP41; rickP41; tom2; tom2.ins

    // Amplitude begins at 2100.6,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 268,268, 27,     53,    53 }, // 296: intP42; rickP42; tom2; tom2.ins

    // Amplitude begins at 2145.4,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 268,268, 29,     53,    53 }, // 297: intP43; rickP43; tom2; tom2.ins

    // Amplitude begins at 2252.7,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 268,268, 32,     53,    53 }, // 298: intP44; rickP44; tom2; tom2.ins

    // Amplitude begins at 1018.4,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 269,269, 32,     13,    13 }, // 299: hamP45; intP45; rickP45; tom; tom.ins

    // Amplitude begins at  837.0,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 270,270, 53,     80,    80 }, // 300: hamP46; intP46; rickP46; conga; conga.in

    // Amplitude begins at  763.1,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 270,270, 57,     80,    80 }, // 301: hamP47; intP47; rickP47; conga; conga.in

    // Amplitude begins at  640.3,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 271,271, 60,     80,    80 }, // 302: hamP48; intP48; rickP48; snare01r

    // Amplitude begins at  969.1,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 272,272, 55,    153,   153 }, // 303: hamP49; intP49; rickP49; slap; slap.ins

    // Amplitude begins at  801.4, peaks  829.5 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 254,254, 85,    260,   260 }, // 304: hamP50; intP50; rickP50; shot; shot.ins

    // Amplitude begins at  882.9, peaks  890.9 at 0.0s,
    // fades to 20% at 0.5s, keyoff fades to 20% in 0.5s.
    { 273,273, 90,    473,   473 }, // 305: hamP51; intP51; rickP51; snrsust; snrsust.

    // Amplitude begins at  766.4,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 274,274, 84,     73,    73 }, // 306: intP52; rickP52; snare; snare.in

    // Amplitude begins at  791.4,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 275,275, 48,    166,   166 }, // 307: hamP53; intP53; rickP53; synsnar; synsnar.

    // Amplitude begins at  715.6,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 276,276, 48,     40,    40 }, // 308: hamP54; intP54; rickP54; synsnr1; synsnr1.

    // Amplitude begins at  198.4,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 132,132, 72,     13,    13 }, // 309: hamP55; intP55; rickP55; aps042; aps042.i

    // Amplitude begins at 1290.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 277,277, 72,     13,    13 }, // 310: hamP56; intP56; rickP56; rimshotb

    // Amplitude begins at  931.4,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 278,278, 72,     13,    13 }, // 311: hamP57; intP57; rickP57; rimshot; rimshot.

    // Amplitude begins at  413.6,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 279,279, 63,    580,   580 }, // 312: hamP58; intP58; rickP58; crash; crash.in

    // Amplitude begins at  407.6,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 279,279, 65,    586,   586 }, // 313: hamP59; intP59; rickP59; crash; crash.in

    // Amplitude begins at  377.2, peaks  377.3 at 0.0s,
    // fades to 20% at 0.5s, keyoff fades to 20% in 0.5s.
    { 280,280, 79,    506,   506 }, // 314: intP60; rickP60; cymbal; cymbal.i

    // Amplitude begins at  102.7, peaks  423.1 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 281,281, 38,    113,   113 }, // 315: hamP61; intP61; rickP61; cymbals; cymbals.

    // Amplitude begins at  507.3,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 282,282, 94,    100,   100 }, // 316: hamP62; intP62; rickP62; hammer5r

    // Amplitude begins at  640.3,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 283,283, 87,    120,   120 }, // 317: hamP63; intP63; rickP63; hammer3; hammer3.

    // Amplitude begins at  611.8,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 283,283, 94,    106,   106 }, // 318: hamP64; intP64; rickP64; hammer3; hammer3.

    // Amplitude begins at  861.4,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 284,284, 80,     60,    60 }, // 319: hamP65; intP65; rickP65; ride2; ride2.in

    // Amplitude begins at  753.6,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 285,285, 47,    140,   140 }, // 320: hamP66; intP66; rickP66; hammer1; hammer1.

    // Amplitude begins at  747.3,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 286,286, 61,     80,    80 }, // 321: intP67; rickP67; tambour; tambour.

    // Amplitude begins at  691.2,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 286,286, 68,     73,    73 }, // 322: intP68; rickP68; tambour; tambour.

    // Amplitude begins at  771.0,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 287,287, 61,    160,   160 }, // 323: hamP69; intP69; rickP69; tambou2; tambou2.

    // Amplitude begins at  716.0, peaks  730.0 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 287,287, 68,    133,   133 }, // 324: hamP70; intP70; rickP70; tambou2; tambou2.

    // Amplitude begins at 2362.1,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 268,268, 60,     40,    40 }, // 325: hamP71; intP71; rickP71; woodbloc

    // Amplitude begins at  912.6,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 288,288, 60,     60,    60 }, // 326: hamP72; intP72; rickP72; woodblok

    // Amplitude begins at 1238.6,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 289,289, 36,     53,    53 }, // 327: intP73; rickP73; claves; claves.i

    // Amplitude begins at 1289.5,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 289,289, 60,     40,    40 }, // 328: intP74; rickP74; claves; claves.i

    // Amplitude begins at  885.6,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 290,290, 60,     40,    40 }, // 329: hamP75; intP75; rickP75; claves2; claves2.

    // Amplitude begins at  931.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 291,291, 60,     40,    40 }, // 330: hamP76; intP76; rickP76; claves3; claves3.

    // Amplitude begins at 2083.1,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 292,292, 68,     20,    20 }, // 331: hamP77; intP77; rickP77; clave; clave.in

    // Amplitude begins at 2344.9,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 293,293, 71,     33,    33 }, // 332: hamP78; intP78; rickP78; agogob4; agogob4.

    // Amplitude begins at 2487.2,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 293,293, 72,     33,    33 }, // 333: hamP79; intP79; rickP79; agogob4; agogob4.

    // Amplitude begins at 1824.6, peaks 1952.1 at 0.0s,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    { 294,294,101,   1153,  1153 }, // 334: hamP80; intP80; rickP80; clarion; clarion.

    // Amplitude begins at 2039.7, peaks 2300.6 at 0.0s,
    // fades to 20% at 1.5s, keyoff fades to 20% in 1.5s.
    { 295,295, 36,   1466,  1466 }, // 335: hamP81; intP81; rickP81; trainbel

    // Amplitude begins at 1466.4, peaks 1476.0 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 2.3s.
    { 296,296, 25,  40000,  2280 }, // 336: hamP82; intP82; rickP82; gong; gong.ins

    // Amplitude begins at  495.7, peaks  730.8 at 0.0s,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 297,297, 37,    420,   420 }, // 337: hamP83; intP83; rickP83; kalimbai

    // Amplitude begins at 2307.5,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 298,298, 36,     40,    40 }, // 338: hamP84; intP84; rickP84; xylo1; xylo1.in

    // Amplitude begins at 2435.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 298,298, 41,     40,    40 }, // 339: hamP85; intP85; rickP85; xylo1; xylo1.in

    // Amplitude begins at    0.0, peaks  445.7 at 0.1s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 299,299, 84,     80,    80 }, // 340: hamP86; intP86; rickP86; match; match.in

    // Amplitude begins at    0.0, peaks  840.6 at 0.2s,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    { 300,300, 54,   1173,  1173 }, // 341: hamP87; intP87; rickP87; breathi; breathi.

    // Amplitude begins at    0.0, peaks  847.3 at 0.2s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 301,301, 36,    186,   186 }, // 342: intP88; rickP88; scratch; scratch.

    // Amplitude begins at    0.0, peaks  870.4 at 0.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 302,302, 60,  40000,     0 }, // 343: hamP89; intP89; rickP89; crowd; crowd.in

    // Amplitude begins at  739.8, peaks 1007.1 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 303,303, 37,     60,    60 }, // 344: intP90; rickP90; taiko; taiko.in

    // Amplitude begins at  844.2,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 304,304, 36,     46,    46 }, // 345: hamP91; intP91; rickP91; rlog; rlog.ins

    // Amplitude begins at 1100.9, peaks 1403.9 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 305,305, 32,     40,    40 }, // 346: hamP92; intP92; rickP92; knock; knock.in

    // Amplitude begins at    0.0, peaks  833.9 at 1.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 249,249, 48,  40000,     0 }, // 347: intP93; drevcym

    // Amplitude begins at 2453.1, peaks 2577.6 at 0.0s,
    // fades to 20% at 2.4s, keyoff fades to 20% in 2.4s.
    { 306,306,  0,   2440,  2440 }, // 348: f35GM88; hamM52; rickM94; Fantasy1; Pad 1 new age; fantasy1

    // Amplitude begins at 1016.1, peaks 1517.4 at 0.0s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 307,307,  0,    573,   573 }, // 349: f35GM24; hamM53; Acoustic Guitar1; guitar1

    // Amplitude begins at 1040.8, peaks 1086.3 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.0s.
    { 308,308,  0,    240,     6 }, // 350: hamM55; hamatmos

    // Amplitude begins at    0.4, peaks 3008.2 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 309,309,  0,  40000,    26 }, // 351: f35GM82; hamM56; Lead 3 calliope; hamcalio

    // Amplitude begins at 1031.7, peaks 1690.4 at 0.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 310,310,  0,  40000,     6 }, // 352: MGM37; b41M37; f19GM37; f23GM37; f32GM37; f35GM84; f41GM37; hamM59; oGM37; Lead 5 charang; Slap Bass 2; moon; moon.ins

    // Amplitude begins at    4.0, peaks 1614.7 at 0.0s,
    // fades to 20% at 4.3s, keyoff fades to 20% in 0.0s.
    { 311,311,  0,   4260,    13 }, // 353: hamM62; Polyham3

    // Amplitude begins at    7.3, peaks 1683.7 at 0.1s,
    // fades to 20% at 4.1s, keyoff fades to 20% in 4.1s.
    { 312,312,  0,   4133,  4133 }, // 354: hamM63; Polyham

    // Amplitude begins at 1500.1, peaks 1587.3 at 0.0s,
    // fades to 20% at 4.8s, keyoff fades to 20% in 4.8s.
    { 313,313,  0,   4826,  4826 }, // 355: f35GM104; hamM64; Sitar; sitar2

    // Amplitude begins at 1572.2, peaks 1680.2 at 0.0s,
    // fades to 20% at 1.8s, keyoff fades to 20% in 1.8s.
    { 314,314,  0,   1753,  1753 }, // 356: f35GM81; hamM70; Lead 2 sawtooth; weird1a

    // Amplitude begins at 1374.4, peaks 1624.9 at 0.0s,
    // fades to 20% at 4.2s, keyoff fades to 20% in 0.0s.
    { 315,315,  0,   4180,    13 }, // 357: hamM71; Polyham4

    // Amplitude begins at 3264.2, peaks 3369.0 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 316,316,  0,  40000,    13 }, // 358: hamM72; hamsynbs

    // Amplitude begins at 1730.5, peaks 2255.7 at 20.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 317,317,  0,  40000,    66 }, // 359: hamM73; Ocasynth

    // Amplitude begins at 1728.4, peaks 3093.1 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 318,318,  0,    126,   126 }, // 360: hamM120; hambass1

    // Amplitude begins at  647.4, peaks  662.4 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 319,319,  0,  40000,    13 }, // 361: hamM121; hamguit1

    // Amplitude begins at 2441.6, peaks 2524.9 at 0.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.6s.
    { 320,320,  0,  40000,   560 }, // 362: hamM122; hamharm2

    // Amplitude begins at    0.3, peaks 4366.8 at 20.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.7s.
    { 321,321,  0,  40000,   666 }, // 363: hamM123; hamvox1

    // Amplitude begins at    0.0, peaks 2319.7 at 2.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.5s.
    { 322,322,  0,  40000,   500 }, // 364: hamM124; hamgob1

    // Amplitude begins at    0.8, peaks 2668.3 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 323,323,  0,  40000,    40 }, // 365: hamM125; hamblow1

    // Amplitude begins at  338.4, peaks  390.0 at 0.0s,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    { 135,135, 49,   1153,  1153 }, // 366: hamP0; crash1

    // Amplitude begins at 1552.4,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 131,131, 47,    106,   106 }, // 367: b45P41; hamP1; aps041

    // Amplitude begins at 1387.8,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 131,131, 49,    153,   153 }, // 368: b45P43; hamP2; aps041

    // Amplitude begins at 1571.5,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 131,131, 51,    153,   153 }, // 369: b45P45; hamP3; aps041

    // Amplitude begins at 1793.9, peaks 1833.4 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 131,131, 54,    113,   113 }, // 370: b45P47; hamP4; aps041

    // Amplitude begins at 1590.0, peaks 1898.7 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 131,131, 57,    140,   140 }, // 371: b45P48; hamP5; aps041

    // Amplitude begins at 1690.3, peaks 1753.1 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 131,131, 60,    160,   160 }, // 372: b45P50; hamP6; aps041

    // Amplitude begins at    3.9, peaks  349.8 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 165,165, 72,     40,    40 }, // 373: hamP7; aps082

    // Amplitude begins at  839.1,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 148,148, 71,    133,   133 }, // 374: b45P65; hamP8; rickP98; aps065; timbale; timbale.

    // Amplitude begins at  825.7,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 324,324, 72,     53,    53 }, // 375: hamP9; cowbell

    // Amplitude begins at 1824.5,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 325,325, 74,     33,    33 }, // 376: hamP10; rickP100; conghi; conghi.i

    // Amplitude begins at 1345.6,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 127,127, 35,     53,    53 }, // 377: b45P0; b45P1; b45P10; b45P11; b45P12; b45P13; b45P14; b45P15; b45P16; b45P17; b45P18; b45P19; b45P2; b45P20; b45P21; b45P22; b45P23; b45P24; b45P25; b45P26; b45P27; b45P28; b45P29; b45P3; b45P30; b45P31; b45P32; b45P33; b45P34; b45P35; b45P36; b45P4; b45P5; b45P6; b45P7; b45P8; b45P9; b46P35; hamP11; rickP14; aps035; gps035; kick2.in

    // Amplitude begins at 1672.4, peaks 2077.7 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 326,326, 35,     40,    40 }, // 378: hamP12; rickP15; hamkick; kick3.in

    // Amplitude begins at 1250.1,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 327,327, 41,      6,     6 }, // 379: hamP13; rimshot2

    // Amplitude begins at  845.2,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 328,328, 38,    100,   100 }, // 380: hamP14; rickP16; hamsnr1; snr1.ins

    // Amplitude begins at  771.4,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 329,329, 39,     26,    26 }, // 381: hamP15; handclap

    // Amplitude begins at  312.7,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 330,330, 49,     40,    40 }, // 382: hamP16; smallsnr

    // Amplitude begins at  914.5, peaks 2240.8 at 15.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 331,331, 83,  40000,     0 }, // 383: hamP17; rickP95; clsdhhat

    // Amplitude begins at 1289.5, peaks 1329.8 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.1s.
    { 332,332, 59,  40000,  1093 }, // 384: hamP18; openhht2

    // Amplitude begins at  648.1, peaks  699.9 at 0.0s,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 137,137, 84,    406,   406 }, // 385: b45P52; hamP19; aps052

    // Amplitude begins at 2642.4, peaks 2809.2 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 333,333, 24,     93,    93 }, // 386: hamP41; tom2

    // Amplitude begins at 2645.3, peaks 3034.8 at 0.1s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 333,333, 27,    133,   133 }, // 387: hamP42; tom2

    // Amplitude begins at 2593.0, peaks 2757.3 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.0s.
    { 333,333, 29,    126,    13 }, // 388: hamP43; tom2

    // Amplitude begins at 2513.8, peaks 3102.7 at 0.1s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.0s.
    { 333,333, 32,    126,    13 }, // 389: hamP44; tom2

    // Amplitude begins at  827.9,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 334,334, 84,    113,   113 }, // 390: hamP52; snare

    // Amplitude begins at  252.7, peaks  435.2 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 281,281, 79,     73,    73 }, // 391: hamP60; cymbal

    // Amplitude begins at   21.4, peaks  414.1 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 335,335, 61,    166,   166 }, // 392: hamP67; tambour

    // Amplitude begins at   53.7, peaks  464.6 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 335,335, 68,    140,   140 }, // 393: hamP68; tambour

    // Amplitude begins at  901.2,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 336,336, 36,     33,    33 }, // 394: hamP73; claves

    // Amplitude begins at  729.5,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 336,336, 60,     20,    20 }, // 395: hamP74; claves

    // Amplitude begins at    0.0, peaks  834.2 at 0.2s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 337,337, 36,    186,   186 }, // 396: hamP88; scratch

    // Amplitude begins at 1400.8, peaks 1786.0 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 115,115, 37,     73,    73 }, // 397: hamP90; taiko

    // Amplitude begins at 1209.9,
    // fades to 20% at 1.1s, keyoff fades to 20% in 1.1s.
    { 338,338,  0,   1126,  1126 }, // 398: rickM76; Bass.ins

    // Amplitude begins at  852.4,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 339,339,  0,    286,   286 }, // 399: f35GM36; rickM77; Basnor04; Slap Bass 1

    // Amplitude begins at    2.4, peaks 1046.0 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 340,340,  0,  40000,     0 }, // 400: b41M39; f32GM39; f41GM39; rickM78; Synbass1; Synth Bass 2; synbass1

    // Amplitude begins at 1467.7, peaks 1896.9 at 0.0s,
    // fades to 20% at 2.0s, keyoff fades to 20% in 2.0s.
    { 341,341,  0,   1973,  1973 }, // 401: rickM79; Synbass2

    // Amplitude begins at 1146.6, peaks 1164.1 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 342,342,  0,    133,   133 }, // 402: f35GM34; rickM80; Electric Bass 2; Pickbass

    // Amplitude begins at 1697.4, peaks 2169.9 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 343,343,  0,  40000,     0 }, // 403: rickM82; Harpsi1.

    // Amplitude begins at 1470.4, peaks 1727.4 at 4.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 344,344,  0,  40000,    33 }, // 404: rickM83; Guit_el3

    // Amplitude begins at    0.3, peaks 1205.9 at 0.1s,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 345,345,  0,    400,   400 }, // 405: rickM88; Orchit2.

    // Amplitude begins at   36.8, peaks 4149.2 at 0.1s,
    // fades to 20% at 2.9s, keyoff fades to 20% in 2.9s.
    { 346,346,  0,   2920,  2920 }, // 406: f35GM61; rickM89; Brass Section; Brass11.

    // Amplitude begins at   42.9, peaks 3542.1 at 0.1s,
    // fades to 20% at 1.5s, keyoff fades to 20% in 1.5s.
    { 347,347,  0,   1520,  1520 }, // 407: f12GM62; f16GM62; f47GM61; f53GM89; f54GM62; rickM90; Brass Section; Brass2.i; Pad 2 warm; Synth Brass 1

    // Amplitude begins at   29.8, peaks 2195.8 at 0.1s,
    // fades to 20% at 3.9s, keyoff fades to 20% in 3.9s.
    { 348,348,  0,   3853,  3853 }, // 408: f12GM61; f16GM61; f37GM61; f47GM63; f54GM61; rickM91; Brass Section; Brass3.i; Synth Brass 2

    // Amplitude begins at  251.3,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 349,349,  0,      6,     6 }, // 409: rickM92; Squ_wave

    // Amplitude begins at 3164.4,
    // fades to 20% at 2.3s, keyoff fades to 20% in 2.3s.
    { 350,350,  0,   2313,  2313 }, // 410: rickM99; Agogo.in

    // Amplitude begins at 1354.8,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 351,351, 35,     66,    66 }, // 411: rickP13; kick1.in

    // Amplitude begins at  879.9,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 352,352, 38,     53,    53 }, // 412: rickP17; rickP19; snare1.i; snare4.i

    // Amplitude begins at  672.5, peaks  773.2 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 353,353, 38,    106,   106 }, // 413: rickP18; rickP20; snare2.i; snare5.i

    // Amplitude begins at 1555.8,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 354,354, 31,     26,    26 }, // 414: rickP21; rocktom.

    // Amplitude begins at 2153.9,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 354,354, 35,     40,    40 }, // 415: rickP22; rocktom.

    // Amplitude begins at 2439.5,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 354,354, 38,     33,    33 }, // 416: rickP23; rocktom.

    // Amplitude begins at 2350.4,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 354,354, 41,     40,    40 }, // 417: rickP24; rocktom.

    // Amplitude begins at 2086.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 354,354, 45,     40,    40 }, // 418: rickP25; rocktom.

    // Amplitude begins at 2389.8,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 354,354, 50,     53,    53 }, // 419: rickP26; rocktom.

    // Amplitude begins at    0.0, peaks  984.9 at 34.8s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 4.6s.
    { 355,355, 50,  40000,  4553 }, // 420: rickP93; openhht1

    // Amplitude begins at 1272.4, peaks 1316.2 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.1s.
    { 332,332, 50,  40000,  1100 }, // 421: rickP94; openhht2

    // Amplitude begins at    1.8, peaks  475.1 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 156,156, 44,     80,    80 }, // 422: rickP96; guiros.i

    // Amplitude begins at 2276.8,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 356,356, 72,     93,    93 }, // 423: rickP97; guirol.i

    // Amplitude begins at  878.1,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 148,148, 59,    153,   153 }, // 424: rickP99; timbale.

    // Amplitude begins at 1799.7,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 357,357, 64,     40,    40 }, // 425: rickP101; congas2.

    // Amplitude begins at 1958.5,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 357,357, 60,     33,    33 }, // 426: rickP102; congas2.

    // Amplitude begins at 2056.5,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 358,358, 72,     26,    26 }, // 427: rickP103; bongos.i

    // Amplitude begins at 1952.7,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 358,358, 62,     33,    33 }, // 428: rickP104; bongos.i

    // Amplitude begins at 1783.9,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 131,131, 53,    120,   120 }, // 429: b45P100; b45P101; b45P102; b45P103; b45P104; b45P105; b45P106; b45P107; b45P108; b45P109; b45P110; b45P111; b45P112; b45P113; b45P114; b45P115; b45P116; b45P117; b45P118; b45P119; b45P120; b45P121; b45P122; b45P123; b45P124; b45P125; b45P126; b45P127; b45P87; b45P88; b45P89; b45P90; b45P91; b45P92; b45P93; b45P94; b45P95; b45P96; b45P97; b45P98; b45P99; rickP105; aps087; surdu.in

    // Amplitude begins at  616.5,
    // fades to 20% at 1.3s, keyoff fades to 20% in 1.3s.
    { 359,359,  0,   1260,  1260 }, // 430: dMM0; hxMM0; Acoustic Grand Piano

    // Amplitude begins at  674.5,
    // fades to 20% at 1.7s, keyoff fades to 20% in 1.7s.
    { 360,360,  0,   1733,  1733 }, // 431: dMM1; hxMM1; Bright Acoustic Piano

    // Amplitude begins at 2354.8, peaks 2448.9 at 0.0s,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.9s.
    { 361,362,  0,    873,   873 }, // 432: dMM2; hxMM2; Electric Grand Piano

    // Amplitude begins at 4021.1, peaks 5053.2 at 0.0s,
    // fades to 20% at 1.3s, keyoff fades to 20% in 1.3s.
    { 363,364,  0,   1286,  1286 }, // 433: dMM3; hxMM3; Honky-tonk Piano

    // Amplitude begins at 1643.0,
    // fades to 20% at 1.3s, keyoff fades to 20% in 1.3s.
    { 365,365,  0,   1286,  1286 }, // 434: dMM4; hxMM4; Rhodes Paino

    // Amplitude begins at 4033.5, peaks 5133.2 at 0.0s,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    { 366,367,  0,   1240,  1240 }, // 435: dMM5; hxMM5; Chorused Piano

    // Amplitude begins at 1822.4, peaks 2092.2 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.1s.
    { 368,368,  0,  40000,  1133 }, // 436: dMM6; hxMM6; Harpsichord

    // Amplitude begins at  550.6, peaks  630.4 at 0.0s,
    // fades to 20% at 1.0s, keyoff fades to 20% in 1.0s.
    { 369,369,  0,    960,   960 }, // 437: dMM7; hxMM7; Clavinet

    // Amplitude begins at 2032.6, peaks 2475.9 at 0.0s,
    // fades to 20% at 1.4s, keyoff fades to 20% in 1.4s.
    { 370,370,  0,   1433,  1433 }, // 438: dMM8; hxMM8; Celesta

    // Amplitude begins at   59.9, peaks 1075.0 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 371,371,  0,    153,   153 }, // 439: dMM9; hxMM9; * Glockenspiel

    // Amplitude begins at 3882.8,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.9s.
    { 372,372,  0,    906,   906 }, // 440: dMM10; hxMM10; * Music Box

    // Amplitude begins at 2597.9, peaks 2725.4 at 0.0s,
    // fades to 20% at 1.5s, keyoff fades to 20% in 1.5s.
    { 373,373,  0,   1460,  1460 }, // 441: dMM11; hxMM11; Vibraphone

    // Amplitude begins at 1033.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 374,374,  0,     40,    40 }, // 442: dMM12; hxMM12; Marimba

    // Amplitude begins at 2928.9,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 375,375,  0,     53,    53 }, // 443: dMM13; hxMM13; Xylophone

    // Amplitude begins at 1140.9, peaks 2041.9 at 0.0s,
    // fades to 20% at 0.5s, keyoff fades to 20% in 0.5s.
    { 376,376,  0,    466,   466 }, // 444: dMM14; hxMM14; * Tubular-bell

    // Amplitude begins at 1166.8, peaks 3114.0 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.6s.
    { 377,377,  0,  40000,   580 }, // 445: dMM15; hxMM15; * Dulcimer

    // Amplitude begins at  854.0, peaks  939.4 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 378,378,  0,  40000,    53 }, // 446: dMM16; hxMM16; Hammond Organ

    // Amplitude begins at 1555.5,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 379,379,  0,    226,   226 }, // 447: dMM17; hxMM17; Percussive Organ

    // Amplitude begins at 2503.7, peaks 3236.9 at 2.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 380,381,  0,  40000,    40 }, // 448: dMM18; hxMM18; Rock Organ

    // Amplitude begins at    0.7, peaks  722.3 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 382,382,  0,  40000,   186 }, // 449: dMM19; hxMM19; Church Organ

    // Amplitude begins at   77.6, peaks 2909.7 at 9.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 383,383,  0,  40000,    66 }, // 450: dMM20; hxMM20; Reed Organ

    // Amplitude begins at    0.0, peaks 1059.3 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 384,385,  0,  40000,    26 }, // 451: dMM21; hxMM21; Accordion

    // Amplitude begins at    0.0, peaks 1284.1 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 386,386,  0,  40000,    46 }, // 452: dMM22; hxMM22; Harmonica

    // Amplitude begins at    2.6, peaks 1923.6 at 8.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 387,388,  0,  40000,   106 }, // 453: dMM23; hxMM23; Tango Accordion

    // Amplitude begins at 4825.0, peaks 6311.8 at 0.0s,
    // fades to 20% at 2.8s, keyoff fades to 20% in 2.8s.
    { 389,389,  0,   2766,  2766 }, // 454: dMM24; hxMM24; Acoustic Guitar (nylon)

    // Amplitude begins at 2029.6, peaks 2275.6 at 0.0s,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.9s.
    { 390,390,  0,    940,   940 }, // 455: dMM25; hxMM25; Acoustic Guitar (steel)

    // Amplitude begins at 1913.7,
    // fades to 20% at 1.1s, keyoff fades to 20% in 1.1s.
    { 391,391,  0,   1060,  1060 }, // 456: dMM26; hxMM26; Electric Guitar (jazz)

    // Amplitude begins at 1565.5,
    // fades to 20% at 0.7s, keyoff fades to 20% in 0.7s.
    { 392,392,  0,    680,   680 }, // 457: dMM27; hxMM27; * Electric Guitar (clean)

    // Amplitude begins at 2719.9,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.9s.
    { 393,393,  0,    866,   866 }, // 458: dMM28; hxMM28; Electric Guitar (muted)

    // Amplitude begins at  662.3, peaks  763.4 at 5.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 394,394,  0,  40000,     0 }, // 459: dMM29; Overdriven Guitar

    // Amplitude begins at 2816.8, peaks 3292.7 at 0.0s,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    { 395,395,  0,   1226,  1226 }, // 460: dMM30; Distortion Guitar

    // Amplitude begins at 3191.9, peaks 3287.9 at 0.0s,
    // fades to 20% at 1.7s, keyoff fades to 20% in 1.7s.
    { 396,396,  0,   1653,  1653 }, // 461: dMM31; hxMM31; * Guitar Harmonics

    // Amplitude begins at 2762.8, peaks 2935.2 at 0.0s,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 397,397,  0,    413,   413 }, // 462: dMM32; Acoustic Bass

    // Amplitude begins at 1228.1, peaks 1273.0 at 0.0s,
    // fades to 20% at 1.7s, keyoff fades to 20% in 1.7s.
    { 398,398,  0,   1720,  1720 }, // 463: dMM33; hxMM33; Electric Bass (finger)

    // Amplitude begins at 2291.3, peaks 2717.4 at 0.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.5s.
    { 399,399,  0,  40000,   493 }, // 464: dMM34; hxMM34; Electric Bass (pick)

    // Amplitude begins at  950.8, peaks 3651.4 at 0.3s,
    // fades to 20% at 2.6s, keyoff fades to 20% in 2.6s.
    { 400,401,  0,   2620,  2620 }, // 465: dMM35; Fretless Bass

    // Amplitude begins at 2558.1, peaks 2881.9 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 402,402,  0,  40000,     6 }, // 466: dMM36; * Slap Bass 1

    // Amplitude begins at 1474.2, peaks 1576.9 at 0.0s,
    // fades to 20% at 1.7s, keyoff fades to 20% in 1.7s.
    { 403,403,  0,   1693,  1693 }, // 467: dMM37; hxMM37; Slap Bass 2

    // Amplitude begins at 1147.4, peaks 1232.1 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 404,404,  0,    233,   233 }, // 468: dMM38; hxMM38; Synth Bass 1

    // Amplitude begins at  803.7, peaks 2475.1 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 405,405,  0,  40000,    13 }, // 469: dMM39; hxMM39; Synth Bass 2

    // Amplitude begins at    0.3, peaks 2321.0 at 0.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 406,406,  0,  40000,   106 }, // 470: dMM40; hxMM40; Violin

    // Amplitude begins at   23.1, peaks 3988.7 at 12.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 407,407,  0,  40000,   160 }, // 471: dMM41; hxMM41; Viola

    // Amplitude begins at 1001.0, peaks 1631.3 at 0.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 408,408,  0,  40000,   193 }, // 472: dMM42; hxMM42; Cello

    // Amplitude begins at  570.1, peaks 1055.7 at 24.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.3s.
    { 409,409,  0,  40000,   260 }, // 473: dMM43; hxMM43; Contrabass

    // Amplitude begins at 1225.0, peaks 4124.2 at 32.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.4s.
    { 410,411,  0,  40000,   386 }, // 474: dMM44; hxMM44; Tremolo Strings

    // Amplitude begins at 1088.5,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 412,412,  0,     13,    13 }, // 475: dMM45; hxMM45; Pizzicato Strings

    // Amplitude begins at 2701.6, peaks 2794.5 at 0.1s,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    { 413,413,  0,   1193,  1193 }, // 476: dMM46; hxMM46; Orchestral Harp

    // Amplitude begins at 1102.2, peaks 1241.9 at 0.0s,
    // fades to 20% at 0.7s, keyoff fades to 20% in 0.7s.
    { 414,414,  0,    680,   680 }, // 477: dMM47; hxMM47; * Timpani

    // Amplitude begins at 1149.7, peaks 2522.2 at 36.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 415,416,  0,  40000,   193 }, // 478: dMM48; hxMM48; String Ensemble 1

    // Amplitude begins at  132.3, peaks 2492.8 at 25.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.3s.
    { 417,418,  0,  40000,   266 }, // 479: dMM49; hxMM49; String Ensemble 2

    // Amplitude begins at    3.2, peaks 1845.6 at 32.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.5s.
    { 419,419,  0,  40000,   480 }, // 480: dMM50; hxMM50; Synth Strings 1

    // Amplitude begins at    0.0, peaks 3217.2 at 3.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.6s.
    { 420,421,  0,  40000,   646 }, // 481: dMM51; hxMM51; Synth Strings 2

    // Amplitude begins at    7.2, peaks 4972.4 at 32.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.5s.
    { 422,423,  0,  40000,   546 }, // 482: dMM52; hxMM52; Choir Aahs

    // Amplitude begins at 1100.3, peaks 4805.2 at 0.1s,
    // fades to 20% at 2.5s, keyoff fades to 20% in 2.5s.
    { 424,424,  0,   2533,  2533 }, // 483: dMM53; hxMM53; Voice Oohs

    // Amplitude begins at    0.0, peaks 3115.2 at 26.8s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 425,425,  0,  40000,   213 }, // 484: dMM54; hxMM54; Synth Voice

    // Amplitude begins at    1.3, peaks 4556.7 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.6s.
    { 426,427,  0,  40000,   640 }, // 485: dMM55; hxMM55; Orchestra Hit

    // Amplitude begins at    0.9, peaks 1367.0 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 428,428,  0,  40000,    13 }, // 486: dMM56; hxMM56; Trumpet

    // Amplitude begins at    1.8, peaks  873.6 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 429,429,  0,  40000,     0 }, // 487: dMM57; hxMM57; Trombone

    // Amplitude begins at  260.9, peaks  558.8 at 0.0s,
    // fades to 20% at 2.4s, keyoff fades to 20% in 2.4s.
    { 430,430,  0,   2393,  2393 }, // 488: dMM58; Tuba

    // Amplitude begins at   11.2, peaks 1622.8 at 0.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 431,431,  0,  40000,    86 }, // 489: dMM59; hxMM59; Muted Trumpet

    // Amplitude begins at    4.9, peaks 3060.2 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 432,433,  0,  40000,    20 }, // 490: dMM60; hxMM60; French Horn

    // Amplitude begins at    1.3, peaks 1502.1 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 434,435,  0,  40000,    20 }, // 491: dMM61; hxMM61; Brass Section

    // Amplitude begins at  749.0, peaks 1624.5 at 0.1s,
    // fades to 20% at 3.0s, keyoff fades to 20% in 3.0s.
    { 436,437,  0,   2966,  2966 }, // 492: dMM62; hxMM62; Synth Brass 1

    // Amplitude begins at   14.4, peaks  901.7 at 0.0s,
    // fades to 20% at 1.6s, keyoff fades to 20% in 1.6s.
    { 438,439,  0,   1566,  1566 }, // 493: dMM63; hxMM63; Synth Bass 2

    // Amplitude begins at    1.0, peaks  943.3 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 440,440,  0,  40000,    40 }, // 494: dMM64; hxMM64; Soprano Sax

    // Amplitude begins at    2.4, peaks 2220.8 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 441,441,  0,  40000,    86 }, // 495: dMM65; hxMM65; Alto Sax

    // Amplitude begins at   94.5, peaks  726.7 at 30.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 442,442,  0,  40000,     6 }, // 496: dMM66; hxMM66; Tenor Sax

    // Amplitude begins at  178.3, peaks 2252.3 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 443,443,  0,  40000,     6 }, // 497: dMM67; hxMM67; Baritone Sax

    // Amplitude begins at    0.0, peaks 1786.2 at 38.8s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 444,444,  0,  40000,    33 }, // 498: dMM68; hxMM68; Oboe

    // Amplitude begins at 2886.1, peaks 3448.9 at 14.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 445,445,  0,  40000,    73 }, // 499: dMM69; hxMM69; English Horn

    // Amplitude begins at 1493.5, peaks 2801.4 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 446,446,  0,  40000,     6 }, // 500: dMM70; hxMM70; Bassoon

    // Amplitude begins at 1806.2, peaks 2146.7 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 447,447,  0,  40000,    20 }, // 501: dMM71; hxMM71; Clarinet

    // Amplitude begins at    0.0, peaks  745.7 at 0.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 448,448,  0,  40000,     0 }, // 502: dMM72; hxMM72; Piccolo

    // Amplitude begins at    0.0, peaks 3494.5 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 449,449,  0,  40000,    20 }, // 503: dMM73; hxMM73; Flute

    // Amplitude begins at    2.8, peaks 1080.0 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 450,450,  0,  40000,    46 }, // 504: dMM74; hxMM74; Recorder

    // Amplitude begins at    0.0, peaks 1241.2 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 451,451,  0,  40000,    20 }, // 505: dMM75; hxMM75; Pan Flute

    // Amplitude begins at    0.0, peaks 3619.6 at 20.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.4s.
    { 452,452,  0,  40000,   400 }, // 506: dMM76; hxMM76; Bottle Blow

    // Amplitude begins at    0.0, peaks  817.4 at 0.1s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 453,453,  0,    140,   140 }, // 507: dMM77; hxMM77; * Shakuhachi

    // Amplitude begins at    0.0, peaks 4087.8 at 0.2s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.0s.
    { 454,454,  0,    240,     6 }, // 508: dMM78; hxMM78; Whistle

    // Amplitude begins at    1.8, peaks 4167.2 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 455,455,  0,  40000,    73 }, // 509: dMM79; hxMM79; Ocarina

    // Amplitude begins at 1515.8, peaks 3351.0 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 456,457,  0,  40000,    86 }, // 510: dMM80; hxMM80; Lead 1 (square)

    // Amplitude begins at 2949.4, peaks 4563.1 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 458,459,  0,  40000,    60 }, // 511: dMM81; hxMM81; Lead 2 (sawtooth)

    // Amplitude begins at    0.0, peaks  915.0 at 37.5s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 460,460,  0,  40000,    73 }, // 512: dMM82; hxMM82; Lead 3 (calliope)

    // Amplitude begins at 1585.2, peaks 3647.5 at 26.8s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.5s.
    { 461,462,  0,  40000,   513 }, // 513: dMM83; hxMM83; Lead 4 (chiffer)

    // Amplitude begins at  789.1, peaks  848.5 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 463,463,  0,  40000,     6 }, // 514: dMM84; hxMM84; Lead 5 (charang)

    // Amplitude begins at 2971.5, peaks 9330.9 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 464,465,  0,  40000,   246 }, // 515: dMM85; hxMM85; Lead 6 (voice)

    // Amplitude begins at 1121.6, peaks 3864.6 at 0.1s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 466,467,  0,    313,   313 }, // 516: dMM86; hxMM86; Lead 7 (5th sawtooth)

    // Amplitude begins at  263.0, peaks  310.4 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 468,468,  0,  40000,    20 }, // 517: dMM87; dMM88; hxMM87; hxMM88; * Lead 8 (bass & lead)

    // Amplitude begins at    0.0, peaks 2982.5 at 1.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 469,470,  0,  40000,   246 }, // 518: dMM89; hxMM89; Pad 2 (warm)

    // Amplitude begins at 1450.1,
    // fades to 20% at 3.9s, keyoff fades to 20% in 3.9s.
    { 471,471,  0,   3886,  3886 }, // 519: dMM90; hxMM90; Pad 3 (polysynth)

    // Amplitude begins at  121.8, peaks 3944.0 at 0.1s,
    // fades to 20% at 4.2s, keyoff fades to 20% in 4.2s.
    { 472,473,  0,   4193,  4193 }, // 520: dMM91; hxMM91; Pad 4 (choir)

    // Amplitude begins at    0.0, peaks 1612.5 at 0.2s,
    // fades to 20% at 2.5s, keyoff fades to 20% in 2.5s.
    { 474,474,  0,   2473,  2473 }, // 521: dMM92; hxMM92; Pad 5 (bowed glass)

    // Amplitude begins at   83.2, peaks 1154.9 at 1.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 2.1s.
    { 475,475,  0,  40000,  2100 }, // 522: dMM93; hxMM93; Pad 6 (metal)

    // Amplitude begins at  198.7, peaks 3847.6 at 0.2s,
    // fades to 20% at 3.4s, keyoff fades to 20% in 0.0s.
    { 476,401,  0,   3373,     6 }, // 523: dMM94; hxMM94; Pad 7 (halo)

    // Amplitude begins at    5.9, peaks  912.6 at 0.5s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.8s.
    { 477,477,  0,  40000,   840 }, // 524: dMM95; hxMM95; Pad 8 (sweep)

    // Amplitude begins at  978.1,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 478,478,  0,     26,    26 }, // 525: dMM96; hxMM96; FX 1 (rain)

    // Amplitude begins at    0.0, peaks  696.6 at 0.5s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 479,480,  0,  40000,    46 }, // 526: dMM97; hxMM97; FX 2 (soundtrack)

    // Amplitude begins at 1791.1, peaks 2994.3 at 0.0s,
    // fades to 20% at 1.0s, keyoff fades to 20% in 1.0s.
    { 481,481,  0,   1046,  1046 }, // 527: dMM98; hxMM98; * FX 3 (crystal)

    // Amplitude begins at 3717.1, peaks 5220.9 at 0.2s,
    // fades to 20% at 1.9s, keyoff fades to 20% in 1.9s.
    { 482,483,  0,   1866,  1866 }, // 528: dMM99; hxMM99; FX 4 (atmosphere)

    // Amplitude begins at 3835.5, peaks 4843.6 at 0.1s,
    // fades to 20% at 2.2s, keyoff fades to 20% in 2.2s.
    { 484,485,  0,   2220,  2220 }, // 529: dMM100; hxMM100; FX 5 (brightness)

    // Amplitude begins at    0.0, peaks 1268.0 at 0.3s,
    // fades to 20% at 1.2s, keyoff fades to 20% in 0.2s.
    { 486,486,  0,   1160,   186 }, // 530: dMM101; hxMM101; FX 6 (goblin)

    // Amplitude begins at    0.0, peaks 1649.2 at 0.2s,
    // fades to 20% at 0.7s, keyoff fades to 20% in 0.7s.
    { 487,487,  0,    746,   746 }, // 531: dMM102; hxMM102; FX 7 (echo drops)

    // Amplitude begins at    0.0, peaks 1255.5 at 0.3s,
    // fades to 20% at 2.6s, keyoff fades to 20% in 2.6s.
    { 488,488,  0,   2646,  2646 }, // 532: dMM103; hxMM103; * FX 8 (star-theme)

    // Amplitude begins at    0.3, peaks 2711.1 at 0.1s,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    { 489,489,  0,   1206,  1206 }, // 533: dMM104; hxMM104; Sitar

    // Amplitude begins at 1221.5, peaks 2663.6 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.4s.
    { 490,490,  0,  40000,   406 }, // 534: dMM105; hxMM105; Banjo

    // Amplitude begins at 1658.6,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 491,492,  0,    406,   406 }, // 535: dMM106; hxMM106; Shamisen

    // Amplitude begins at 1657.4, peaks 2263.2 at 0.1s,
    // fades to 20% at 3.6s, keyoff fades to 20% in 3.6s.
    { 493,493,  0,   3566,  3566 }, // 536: dMM107; hxMM107; Koto

    // Amplitude begins at 2222.4,
    // fades to 20% at 2.9s, keyoff fades to 20% in 2.9s.
    { 494,494,  0,   2940,  2940 }, // 537: dMM108; hxMM108; Kalimba

    // Amplitude begins at    0.2, peaks  554.9 at 8.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 495,495,  0,  40000,    73 }, // 538: dMM109; hxMM109; Bag Pipe

    // Amplitude begins at 2646.1, peaks 3358.8 at 33.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.4s.
    { 496,496,  0,  40000,   420 }, // 539: dMM110; hxMM110; Fiddle

    // Amplitude begins at    1.4, peaks 2985.9 at 0.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 497,497,  0,  40000,   246 }, // 540: dMM111; hxMM111; Shanai

    // Amplitude begins at 1438.2, peaks 1485.7 at 0.0s,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    { 498,498,  0,   1220,  1220 }, // 541: dMM112; hxMM112; Tinkle Bell

    // Amplitude begins at 3327.5, peaks 3459.2 at 0.0s,
    // fades to 20% at 1.4s, keyoff fades to 20% in 1.4s.
    { 499,499,  0,   1440,  1440 }, // 542: dMM113; hxMM113; Agogo

    // Amplitude begins at 1245.4, peaks 2279.8 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 500,500,  0,  40000,   213 }, // 543: dMM114; hxMM114; Steel Drums

    // Amplitude begins at 1567.6,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 501,501,  0,    113,   113 }, // 544: dMM115; hxMM115; Woodblock

    // Amplitude begins at 2897.0, peaks 3350.1 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 502,502,  0,     40,    40 }, // 545: dMM116; hxMM116; Taiko Drum

    // Amplitude begins at 3180.9, peaks 3477.5 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 503,503,  0,    120,   120 }, // 546: dMM117; hxMM117; Melodic Tom

    // Amplitude begins at 4290.3, peaks 4378.4 at 0.0s,
    // fades to 20% at 0.7s, keyoff fades to 20% in 0.7s.
    { 504,504,  0,    680,   680 }, // 547: dMM118; hxMM118; Synth Drum

    // Amplitude begins at    0.0, peaks 2636.1 at 0.4s,
    // fades to 20% at 4.6s, keyoff fades to 20% in 4.6s.
    { 505,505,  0,   4586,  4586 }, // 548: dMM119; hxMM119; Reverse Cymbal

    // Amplitude begins at    0.3, peaks 1731.3 at 0.2s,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 506,506,  0,    426,   426 }, // 549: dMM120; hxMM120; Guitar Fret Noise

    // Amplitude begins at    0.0, peaks 3379.5 at 0.2s,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 507,507,  0,    360,   360 }, // 550: dMM121; hxMM121; Breath Noise

    // Amplitude begins at    0.0, peaks  365.3 at 2.3s,
    // fades to 20% at 2.9s, keyoff fades to 20% in 2.9s.
    { 508,508,  0,   2926,  2926 }, // 551: dMM122; hxMM122; Seashore

    // Amplitude begins at    0.0, peaks 1372.7 at 0.1s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 509,509,  0,    306,   306 }, // 552: dMM123; hxMM123; Bird Tweet

    // Amplitude begins at  708.2, peaks  797.4 at 30.5s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 510,510,  0,  40000,    73 }, // 553: dMM124; hxMM124; Telephone Ring

    // Amplitude begins at    0.0, peaks  764.4 at 1.7s,
    // fades to 20% at 1.7s, keyoff fades to 20% in 0.0s.
    { 511,511, 17,   1686,     6 }, // 554: dMM125; hxMM125; Helicopter

    // Amplitude begins at    0.0, peaks  356.0 at 27.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.9s.
    { 512,512, 65,  40000,  1873 }, // 555: dMM126; hxMM126; Applause

    // Amplitude begins at  830.5,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 513,513,  0,    160,   160 }, // 556: dMM127; hxMM127; Gun Shot

    // Amplitude begins at 1420.1,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 514,514, 38,     60,    60 }, // 557: dMP35; hxMP35; Acoustic Bass Drum

    // Amplitude begins at 1343.2, peaks 1616.4 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 515,515, 25,     33,    33 }, // 558: dMP36; hxMP36; Acoustic Bass Drum

    // Amplitude begins at 1457.9,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 516,516, 83,     60,    60 }, // 559: dMP37; hxMP37; Slide Stick

    // Amplitude begins at 1244.0,
    // fades to 20% at 0.5s, keyoff fades to 20% in 0.5s.
    { 517,517, 32,    506,   506 }, // 560: dMP38; hxMP38; Acoustic Snare

    // Amplitude begins at  339.2,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 518,518, 60,     40,    40 }, // 561: dMP39; hxMP39; Hand Clap

    // Amplitude begins at  707.9,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 519,520, 36,     46,    46 }, // 562: dMP40; Electric Snare

    // Amplitude begins at  867.5,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 521,521, 15,     73,    73 }, // 563: dMP41; hxMP41; Low Floor Tom

    // Amplitude begins at  396.4,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 522,522, 88,    113,   113 }, // 564: dMP42; hxMP42; Closed High-Hat

    // Amplitude begins at  779.3,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 523,523, 19,    100,   100 }, // 565: dMP43; hxMP43; High Floor Tom

    // Amplitude begins at   77.1, peaks  358.4 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 524,524, 88,     20,    20 }, // 566: dMP44; dMP69; hxMP44; hxMP69; Cabasa

    // Amplitude begins at 1217.1,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 525,525, 21,     93,    93 }, // 567: dMP45; hxMP45; Low Tom

    // Amplitude begins at 1359.4,
    // fades to 20% at 3.3s, keyoff fades to 20% in 3.3s.
    { 526,526, 79,   3346,  3346 }, // 568: dMP46; hxMP46; Open High Hat

    // Amplitude begins at 1204.2,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 525,525, 26,    100,   100 }, // 569: dMP47; hxMP47; Low-Mid Tom

    // Amplitude begins at 1197.1,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 525,525, 28,    100,   100 }, // 570: dMP48; hxMP48; High-Mid Tom

    // Amplitude begins at  391.2,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 527,527, 60,    293,   293 }, // 571: dMP49; dMP57; hxMP49; hxMP57; Crash Cymbal 1

    // Amplitude begins at 1166.5,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 525,525, 32,     80,    80 }, // 572: dMP50; hxMP50; High Tom

    // Amplitude begins at  475.1,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 528,528, 60,    266,   266 }, // 573: dMP51; dMP59; hxMP51; hxMP59; Ride Cymbal 1

    // Amplitude begins at  348.2,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 529,529, 96,    193,   193 }, // 574: dMP52; hxMP52; Chinses Cymbal

    // Amplitude begins at  426.5,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 530,530, 72,    133,   133 }, // 575: dMP53; hxMP53; Ride Bell

    // Amplitude begins at  289.3, peaks 1225.2 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 531,531, 79,    140,   140 }, // 576: dMP54; hxMP54; Tambourine

    // Amplitude begins at 1313.3,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 532,532, 69,    126,   126 }, // 577: dMP55; Splash Cymbal

    // Amplitude begins at 1403.7,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 533,533, 71,    146,   146 }, // 578: dMP56; hxMP56; Cowbell

    // Amplitude begins at 1222.3,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 534,534,  0,    200,   200 }, // 579: dMP58; hxMP58; Vibraslap

    // Amplitude begins at 1193.8,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 535,535, 60,     93,    93 }, // 580: dMP60; hxMP60; High Bongo

    // Amplitude begins at 1365.5,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 535,535, 54,    106,   106 }, // 581: dMP61; hxMP61; Low Bango

    // Amplitude begins at 2621.3,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 536,536, 72,     66,    66 }, // 582: dMP62; hxMP62; Mute High Conga

    // Amplitude begins at 2596.4,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 536,536, 67,     66,    66 }, // 583: dMP63; hxMP63; Open High Conga

    // Amplitude begins at 2472.4,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 536,536, 60,     66,    66 }, // 584: dMP64; hxMP64; Low Conga

    // Amplitude begins at 2936.4,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 537,537, 55,     66,    66 }, // 585: dMP65; hxMP65; High Timbale

    // Amplitude begins at 3142.8,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 537,537, 48,     80,    80 }, // 586: dMP66; hxMP66; Low Timbale

    // Amplitude begins at 1311.3,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 538,538, 77,    133,   133 }, // 587: dMP67; hxMP67; High Agogo

    // Amplitude begins at 1337.6,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 538,538, 72,    133,   133 }, // 588: dMP68; hxMP68; Low Agogo

    // Amplitude begins at  189.2,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 539,539,  0,      6,     6 }, // 589: dMP70; hxMP70; Maracas

    // Amplitude begins at  252.6,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 540,540, 49,     20,    20 }, // 590: dMP71; dMP72; dMP73; dMP74; dMP79; hxMP71; hxMP72; hxMP73; hxMP74; hxMP79; Long Guiro

    // Amplitude begins at 1479.2,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 541,541, 73,     60,    60 }, // 591: dMP75; hxMP75; Claves

    // Amplitude begins at 1491.4,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 541,541, 68,     60,    60 }, // 592: dMP76; hxMP76; High Wood Block

    // Amplitude begins at 1618.8,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 541,541, 61,     80,    80 }, // 593: dMP77; hxMP77; Low Wood Block

    // Amplitude begins at  189.2, peaks  380.0 at 0.1s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 542,542,  0,    146,   146 }, // 594: dMP78; hxMP78; Mute Cuica

    // Amplitude begins at 1698.0,
    // fades to 20% at 0.8s, keyoff fades to 20% in 0.8s.
    { 543,543, 90,    760,   760 }, // 595: dMP80; hxMP80; Mute Triangle

    // Amplitude begins at 1556.4,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 544,544, 90,    433,   433 }, // 596: dMP81; hxMP81; Open Triangle

    // Amplitude begins at 1237.2,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 545,545,  0,  40000,    40 }, // 597: hxMM29; Overdriven Guitar

    // Amplitude begins at  763.4, peaks  782.5 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 546,546,  0,  40000,    60 }, // 598: hxMM30; Distortion Guitar

    // Amplitude begins at  990.9, peaks 1053.6 at 0.0s,
    // fades to 20% at 2.3s, keyoff fades to 20% in 2.3s.
    { 547,547,  0,   2273,  2273 }, // 599: hxMM32; Acoustic Bass

    // Amplitude begins at  681.3, peaks 1488.4 at 0.0s,
    // fades to 20% at 1.3s, keyoff fades to 20% in 1.3s.
    { 548,548,  0,   1346,  1346 }, // 600: hxMM35; Fretless Bass

    // Amplitude begins at 2940.0, peaks 3034.5 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 549,550,  0,  40000,     6 }, // 601: hxMM36; * Slap Bass 1

    // Amplitude begins at   66.1, peaks  600.7 at 0.0s,
    // fades to 20% at 2.3s, keyoff fades to 20% in 2.3s.
    { 551,551,  0,   2260,  2260 }, // 602: hxMM58; Tuba

    // Amplitude begins at 2159.6,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 552,553, 36,     40,    40 }, // 603: hxMP40; Electric Snare

    // Amplitude begins at 1148.7, peaks 1298.7 at 0.0s,
    // fades to 20% at 0.5s, keyoff fades to 20% in 0.5s.
    { 554,554, 69,    513,   513 }, // 604: hxMP55; Splash Cymbal

    // Amplitude begins at  893.0, peaks  914.4 at 0.0s,
    // fades to 20% at 0.7s, keyoff fades to 20% in 0.7s.
    { 555,555,  0,    660,   660 }, // 605: sGM6; Harpsichord

    // Amplitude begins at 3114.7, peaks 3373.0 at 0.0s,
    // fades to 20% at 0.8s, keyoff fades to 20% in 0.8s.
    { 556,556,  0,    820,   820 }, // 606: sGM9; Glockenspiel

    // Amplitude begins at 1173.2, peaks 1746.9 at 0.0s,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.9s.
    { 557,557,  0,    946,   946 }, // 607: sGM14; Tubular Bells

    // Amplitude begins at  520.4, peaks 1595.5 at 20.8s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 558,558,  0,  40000,   140 }, // 608: sGM19; Church Organ

    // Amplitude begins at 1154.6, peaks 1469.3 at 0.0s,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.9s.
    { 559,559,  0,    906,   906 }, // 609: sGM24; Acoustic Guitar1

    // Amplitude begins at   84.3, peaks 3105.0 at 1.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 560,560,  0,  40000,     0 }, // 610: sGM44; Tremulo Strings

    // Amplitude begins at 2434.5, peaks 2872.0 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 561,561,  0,    206,   206 }, // 611: sGM45; Pizzicato String

    // Amplitude begins at 2497.2, peaks 3945.4 at 0.0s,
    // fades to 20% at 1.6s, keyoff fades to 20% in 1.6s.
    { 562,562,  0,   1633,  1633 }, // 612: sGM46; Orchestral Harp

    // Amplitude begins at 1574.7, peaks 1635.3 at 0.1s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 563,563,  0,    260,   260 }, // 613: sGM47; Timpany

    // Amplitude begins at  362.3, peaks 3088.9 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 564,564,  0,  40000,     0 }, // 614: sGM48; String Ensemble1

    // Amplitude begins at    0.0, peaks  969.5 at 0.7s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 565,565,  0,  40000,   233 }, // 615: sGM49; String Ensemble2

    // Amplitude begins at 2000.6, peaks 3290.8 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.1s.
    { 566,566,  0,  40000,  1106 }, // 616: sGM50; Synth Strings 1

    // Amplitude begins at 1903.9, peaks 3244.2 at 35.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 567,567,  0,  40000,     0 }, // 617: sGM52; Choir Aahs

    // Amplitude begins at  462.4, peaks 2679.8 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 568,568,  0,    126,   126 }, // 618: sGM55; Orchestra Hit

    // Amplitude begins at   42.7, peaks  937.1 at 0.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 569,569,  0,  40000,    40 }, // 619: sGM56; Trumpet

    // Amplitude begins at   49.7, peaks 3958.0 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 570,570,  0,  40000,    73 }, // 620: sGM57; Trombone

    // Amplitude begins at   42.8, peaks 1043.9 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 571,571,  0,  40000,    26 }, // 621: sGM58; Tuba

    // Amplitude begins at    3.1, peaks 1099.6 at 0.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 572,572,  0,  40000,    73 }, // 622: sGM60; French Horn

    // Amplitude begins at   52.8, peaks 3225.9 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 573,573,  0,  40000,    26 }, // 623: sGM61; Brass Section

    // Amplitude begins at   52.0, peaks 1298.7 at 23.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 574,574,  0,  40000,     6 }, // 624: sGM68; Oboe

    // Amplitude begins at  577.9, peaks 1638.8 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 575,575,  0,  40000,     6 }, // 625: sGM70; Bassoon

    // Amplitude begins at    5.6, peaks 1972.5 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 576,576,  0,  40000,    33 }, // 626: sGM71; Clarinet

    // Amplitude begins at   41.5, peaks 3936.5 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 577,577,  0,  40000,     0 }, // 627: sGM72; Piccolo

    // Amplitude begins at    6.8, peaks 2790.5 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 578,578,  0,  40000,    13 }, // 628: sGM73; Flute

    // Amplitude begins at    0.0, peaks  798.5 at 2.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 579,579,  0,  40000,    46 }, // 629: sGM95; Pad 8 sweep

    // Amplitude begins at 1628.4,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 580,580,  0,    120,   120 }, // 630: sGM116; Taiko Drum

    // Amplitude begins at 1481.7,
    // fades to 20% at 2.3s, keyoff fades to 20% in 2.3s.
    { 581,581,  0,   2260,  2260 }, // 631: sGM118; Synth Drum

    // Amplitude begins at    0.0, peaks  488.6 at 2.3s,
    // fades to 20% at 2.4s, keyoff fades to 20% in 2.4s.
    { 582,582,  0,   2366,  2366 }, // 632: sGM119; Reverse Cymbal

    // Amplitude begins at 1523.0, peaks 1718.4 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 583,583, 16,     20,    20 }, // 633: sGP35; sGP36; Ac Bass Drum; Bass Drum 1

    // Amplitude begins at  649.1,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 584,584,  6,      6,     6 }, // 634: sGP37; Side Stick

    // Amplitude begins at 1254.1,
    // fades to 20% at 3.1s, keyoff fades to 20% in 3.1s.
    { 581,581, 14,   3060,  3060 }, // 635: sGP38; sGP40; Acoustic Snare; Electric Snare

    // Amplitude begins at  600.5,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 585,585, 14,     53,    53 }, // 636: sGP39; Hand Clap

    // Amplitude begins at 1914.9,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 586,586,  0,    106,   106 }, // 637: sGP41; sGP43; sGP45; sGP47; sGP48; sGP50; High Floor Tom; High Tom; High-Mid Tom; Low Floor Tom; Low Tom; Low-Mid Tom

    // Amplitude begins at  265.8,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 587,587, 12,     40,    40 }, // 638: sGP42; Closed High Hat

    // Amplitude begins at  267.6,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 588,588, 12,    266,   266 }, // 639: sGP44; Pedal High Hat

    // Amplitude begins at  398.8,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.9s.
    { 589,589, 12,    893,   893 }, // 640: sGP46; Open High Hat

    // Amplitude begins at  133.6, peaks  393.7 at 0.0s,
    // fades to 20% at 0.8s, keyoff fades to 20% in 0.8s.
    { 590,590, 14,    753,   753 }, // 641: sGP49; Crash Cymbal 1

    // Amplitude begins at  180.7, peaks  720.4 at 0.0s,
    // fades to 20% at 1.5s, keyoff fades to 20% in 1.5s.
    { 591,591, 14,   1466,  1466 }, // 642: sGP52; Chinese Cymbal

    // Amplitude begins at   24.4, peaks 1209.1 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 592,592, 14,    113,   113 }, // 643: sGP54; Tambourine

    // Amplitude begins at  362.1,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 593,593, 78,    280,   280 }, // 644: sGP55; Splash Cymbal

    // Amplitude begins at  133.6, peaks  399.9 at 0.0s,
    // fades to 20% at 2.9s, keyoff fades to 20% in 2.9s.
    { 594,594, 14,   2946,  2946 }, // 645: sGP57; Crash Cymbal 2

    // Amplitude begins at  601.4,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 595,595,  0,    126,   126 }, // 646: sGP58; Vibraslap

    // Amplitude begins at    0.2, peaks  358.9 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 596,596, 14,    106,   106 }, // 647: sGP69; Cabasa

    // Amplitude begins at  398.3,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 597,597,  2,      6,     6 }, // 648: sGP76; High Wood Block

    // Amplitude begins at 4477.5, peaks 6751.4 at 0.1s,
    // fades to 20% at 1.8s, keyoff fades to 20% in 1.8s.
    { 598,599,  0,   1800,  1800 }, // 649: b46M0; b47M0; f20GM0; f31GM0; f36GM0; f48GM0; qGM0; AcouGrandPiano; gm000

    // Amplitude begins at 3079.2, peaks 4137.9 at 0.3s,
    // fades to 20% at 1.5s, keyoff fades to 20% in 1.5s.
    { 600,601,  0,   1486,  1486 }, // 650: b46M1; b47M1; f20GM1; f31GM1; f36GM1; f48GM1; qGM1; BrightAcouGrand; gm001

    // Amplitude begins at 1132.6, peaks 1399.6 at 0.0s,
    // fades to 20% at 1.9s, keyoff fades to 20% in 1.9s.
    { 602,603,  0,   1940,  1940 }, // 651: b46M2; b47M2; f20GM2; f31GM2; f36GM2; qGM2; ElecGrandPiano; gm002

    // Amplitude begins at 3269.8, peaks 3838.6 at 0.1s,
    // fades to 20% at 0.7s, keyoff fades to 20% in 0.7s.
    { 604,605,  0,    673,   673 }, // 652: f20GM3; f31GM3; f36GM3; qGM3; Honky-tonkPiano

    // Amplitude begins at 2750.2, peaks 3130.0 at 0.0s,
    // fades to 20% at 1.1s, keyoff fades to 20% in 1.1s.
    { 606,607,  0,   1080,  1080 }, // 653: b46M4; b47M4; f20GM4; f36GM4; f48GM4; f49GM4; qGM4; Rhodes Piano; gm004

    // Amplitude begins at 3945.7, peaks 5578.5 at 0.1s,
    // fades to 20% at 1.1s, keyoff fades to 20% in 1.1s.
    { 608,609,  0,   1133,  1133 }, // 654: b46M5; b47M5; f20GM5; f31GM5; f36GM5; f48GM5; f49GM5; qGM5; Chorused Piano; gm005

    // Amplitude begins at 1928.3, peaks 2129.4 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 610,611,  0,  40000,     6 }, // 655: b46M6; b47M6; f20GM6; f31GM6; f36GM6; f48GM6; f49GM6; qGM6; Harpsichord; gm006

    // Amplitude begins at 2293.1, peaks 2446.0 at 0.0s,
    // fades to 20% at 0.8s, keyoff fades to 20% in 0.8s.
    { 612,613,  0,    780,   780 }, // 656: f20GM7; f31GM7; f36GM7; qGM7; Clavinet

    // Amplitude begins at 5791.8, peaks 6530.2 at 0.0s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 614,615,  0,    626,   626 }, // 657: b46M8; b47M8; f20GM8; f36GM8; qGM8; Celesta; gm008

    // Amplitude begins at 1985.4, peaks 2087.5 at 0.0s,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    { 616,617,  0,   1180,  1180 }, // 658: b46M9; b47M9; f20GM9; f31GM9; f36GM9; qGM9; Glockenspiel; gm009

    // Amplitude begins at 2286.5, peaks 4615.4 at 0.1s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 180,618,  0,    333,   333 }, // 659: b46M10; b47M10; f20GM10; f31GM10; f36GM10; f49GM10; qGM10; Music box; gm010

    // Amplitude begins at 1568.6, peaks 1706.8 at 0.0s,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    { 619,620,  0,   1226,  1226 }, // 660: f20GM11; f36GM11; f48GM11; f49GM11; qGM11; Vibraphone

    // Amplitude begins at 2801.5,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 621,622,  0,    140,   140 }, // 661: b46M12; b47M12; f20GM12; f31GM12; f36GM12; f49GM12; qGM12; Marimba; gm012

    // Amplitude begins at 3397.2,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 623,624,  0,     80,    80 }, // 662: b46M13; b47M13; f20GM13; f31GM13; f36GM13; f49GM13; qGM13; Xylophone; gm013

    // Amplitude begins at 2842.0, peaks 4706.3 at 0.0s,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.9s.
    { 625,626,  0,    893,   893 }, // 663: b46M14; b47M14; f20GM14; f36GM14; f49GM14; qGM14; Tubular Bells; gm014

    // Amplitude begins at    2.6, peaks  923.3 at 0.0s,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 627,628,  0,    426,   426 }, // 664: b46M15; b47M15; f20GM15; f31GM15; f36GM15; f49GM15; qGM15; Dulcimer; gm015

    // Amplitude begins at 1957.2, peaks 2420.9 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 629,630,  0,  40000,     6 }, // 665: b46M16; b47M16; f20GM16; f31GM16; f36GM16; f49GM16; qGM16; Hammond Organ; gm016

    // Amplitude begins at 2298.8, peaks 3038.1 at 0.1s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.0s.
    { 631,632,  0,    160,     6 }, // 666: b46M17; b47M17; f20GM17; f31GM17; f36GM17; f49GM17; qGM17; Percussive Organ; gm017

    // Amplitude begins at  791.1, peaks 2905.2 at 34.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 633,634,  0,  40000,     6 }, // 667: f20GM18; f31GM18; f36GM18; qGM18; Rock Organ

    // Amplitude begins at 1807.8, peaks 2893.6 at 10.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.8s.
    { 635,636,  0,  40000,  1846 }, // 668: b46M19; b47M19; f20GM19; f31GM19; f36GM19; f48GM19; f49GM19; qGM19; Church Organ; gm019

    // Amplitude begins at 2133.4, peaks 2785.7 at 28.5s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 637,638,  0,  40000,   213 }, // 669: b46M20; b47M20; f20GM20; f31GM20; f36GM20; f49GM20; qGM20; Reed Organ; gm020

    // Amplitude begins at    7.9, peaks 3918.1 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 639,640,  0,  40000,     6 }, // 670: b46M21; b47M21; f20GM21; f31GM21; f36GM21; f49GM21; qGM21; Accordion; gm021

    // Amplitude begins at 2247.2, peaks 3389.7 at 7.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 641,642,  0,  40000,   226 }, // 671: b46M22; b47M22; f20GM22; f31GM22; f36GM22; f48GM22; f49GM22; qGM22; Harmonica; gm022

    // Amplitude begins at    0.9, peaks 1919.3 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 643,644,  0,  40000,     6 }, // 672: b46M23; b47M23; f20GM23; f31GM23; f36GM23; f48GM23; f49GM23; qGM23; Tango Accordion; gm023

    // Amplitude begins at 2100.3,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.9s.
    { 645,646,  0,    886,   886 }, // 673: b46M24; b47M24; f20GM24; f31GM24; f36GM24; f48GM24; qGM24; Acoustic Guitar1; gm024

    // Amplitude begins at 3850.9,
    // fades to 20% at 2.0s, keyoff fades to 20% in 2.0s.
    { 647,648,  0,   1966,  1966 }, // 674: b46M25; b47M25; f20GM25; f31GM25; f36GM25; f48GM25; f49GM25; qGM25; Acoustic Guitar2; gm025

    // Amplitude begins at 3321.6, peaks 4754.4 at 0.0s,
    // fades to 20% at 2.0s, keyoff fades to 20% in 2.0s.
    { 649,650,  0,   1993,  1993 }, // 675: b46M26; b47M26; f20GM26; f31GM26; f36GM26; f48GM26; f49GM26; qGM26; Electric Guitar1; gm026

    // Amplitude begins at 2748.4, peaks 3648.4 at 0.0s,
    // fades to 20% at 1.5s, keyoff fades to 20% in 1.5s.
    { 651,652,  0,   1466,  1466 }, // 676: b46M27; b47M27; f20GM27; f31GM27; f36GM27; qGM27; Electric Guitar2; gm027

    // Amplitude begins at 3478.2, peaks 3876.2 at 0.0s,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 653,654,  0,    393,   393 }, // 677: b46M28; b47M28; f20GM28; f31GM28; f36GM28; f48GM28; qGM28; Electric Guitar3; gm028

    // Amplitude begins at  751.6, peaks 3648.5 at 0.0s,
    // fades to 20% at 5.3s, keyoff fades to 20% in 5.3s.
    { 655,656,  0,   5333,  5333 }, // 678: b46M29; b47M29; f20GM29; f31GM29; f36GM29; f49GM1; qGM29; BrightAcouGrand; Overdrive Guitar; gm029

    // Amplitude begins at  927.9, peaks 5296.4 at 0.1s,
    // fades to 20% at 3.1s, keyoff fades to 20% in 3.1s.
    { 657,658,  0,   3053,  3053 }, // 679: b46M30; b47M30; f20GM30; f31GM30; f36GM30; qGM30; Distorton Guitar; gm030

    // Amplitude begins at 2689.5, peaks 2713.1 at 1.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 659,660,  0,  40000,     0 }, // 680: b46M31; b47M31; f20GM31; f31GM31; f36GM31; qGM31; Guitar Harmonics; gm031

    // Amplitude begins at 2624.8, peaks 8922.0 at 0.0s,
    // fades to 20% at 1.5s, keyoff fades to 20% in 1.5s.
    { 661,662,  0,   1520,  1520 }, // 681: b46M32; b47M32; f20GM32; f31GM32; f36GM32; qGM32; Acoustic Bass; gm032

    // Amplitude begins at 3365.5, peaks 4867.6 at 0.0s,
    // fades to 20% at 2.0s, keyoff fades to 20% in 2.0s.
    { 663,664,  0,   1993,  1993 }, // 682: b46M33; b47M33; f20GM33; f31GM33; f36GM33; f49GM39; qGM33; Electric Bass 1; Synth Bass 2; gm033

    // Amplitude begins at 2384.1, peaks 2910.0 at 0.7s,
    // fades to 20% at 2.2s, keyoff fades to 20% in 2.2s.
    { 665,666,  0,   2153,  2153 }, // 683: b46M34; b47M34; f20GM34; f31GM34; f36GM34; qGM34; Electric Bass 2; gm034

    // Amplitude begins at  101.2, peaks 5000.0 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    {  35,667,  0,  40000,     6 }, // 684: b46M35; b47M35; f20GM35; f31GM35; f36GM35; f49GM35; qGM35; Fretless Bass; gm035

    // Amplitude begins at 2970.7, peaks 4061.8 at 0.0s,
    // fades to 20% at 2.4s, keyoff fades to 20% in 2.4s.
    {  36,668,  0,   2446,  2446 }, // 685: b46M36; b47M36; f20GM36; f31GM36; f36GM36; f49GM36; qGM36; Slap Bass 1; gm036

    // Amplitude begins at 3523.2, peaks 5695.3 at 0.0s,
    // fades to 20% at 1.9s, keyoff fades to 20% in 1.9s.
    { 669,670,  0,   1886,  1886 }, // 686: b46M37; b47M37; f20GM37; f31GM37; f36GM37; qGM37; Slap Bass 2; gm037

    // Amplitude begins at 1205.3, peaks 1758.8 at 0.1s,
    // fades to 20% at 2.5s, keyoff fades to 20% in 2.5s.
    { 671,672,  0,   2500,  2500 }, // 687: b46M38; b47M38; f20GM38; f31GM38; f36GM38; qGM38; Synth Bass 1; gm038

    // Amplitude begins at 1685.3, peaks 2295.6 at 0.0s,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    { 673,674,  0,   1220,  1220 }, // 688: b46M39; b47M39; f20GM39; f31GM39; f36GM39; qGM39; Synth Bass 2; gm039

    // Amplitude begins at  951.6, peaks 2721.2 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    {  39,675,  0,  40000,   106 }, // 689: b46M40; b47M40; f20GM40; f31GM40; f36GM40; f48GM40; f49GM40; qGM40; Violin; gm040

    // Amplitude begins at  956.5, peaks 2756.5 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 676,675,  0,  40000,   106 }, // 690: b46M41; b47M41; f20GM41; f31GM41; f36GM41; f49GM41; qGM41; Viola; gm041

    // Amplitude begins at 1099.9, peaks 2643.7 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 677,678,  0,  40000,   106 }, // 691: b46M42; b47M42; f20GM42; f31GM42; f36GM42; f49GM42; qGM42; Cello; gm042

    // Amplitude begins at 1823.4, peaks 3093.6 at 5.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 679,680,  0,  40000,    73 }, // 692: b46M43; b47M43; f20GM43; f31GM43; f36GM43; f48GM43; f49GM43; qGM43; Contrabass; gm043

    // Amplitude begins at 1975.3, peaks 3200.6 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 681,682,  0,  40000,   193 }, // 693: b46M44; b47M44; f20GM44; f31GM44; f36GM44; f49GM44; qGM44; Tremulo Strings; gm044

    // Amplitude begins at  207.0, peaks 3891.5 at 0.0s,
    // fades to 20% at 2.0s, keyoff fades to 20% in 2.0s.
    { 683,684,  0,   2000,  2000 }, // 694: b46M45; b47M45; f20GM45; f36GM45; f49GM45; qGM45; Pizzicato String; gm045

    // Amplitude begins at  207.0, peaks 3923.0 at 0.0s,
    // fades to 20% at 2.0s, keyoff fades to 20% in 2.0s.
    { 683,685,  0,   2000,  2000 }, // 695: b46M46; b47M46; f20GM46; f36GM46; f48GM46; f49GM46; qGM46; Orchestral Harp; gm046

    // Amplitude begins at 1802.1, peaks 4031.7 at 0.1s,
    // fades to 20% at 0.8s, keyoff fades to 20% in 0.8s.
    {  46,686,  0,    833,   833 }, // 696: b46M47; b47M47; f20GM47; f36GM47; qGM47; Timpany; gm047

    // Amplitude begins at    0.0, peaks  701.8 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 687,688,  0,  40000,    60 }, // 697: b46M48; b47M48; f20GM48; f36GM48; f49GM48; qGM48; String Ensemble1; gm048

    // Amplitude begins at    0.0, peaks  502.5 at 0.5s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 689,690,  0,  40000,    60 }, // 698: b46M49; b47M49; f20GM49; f36GM49; f49GM49; qGM49; String Ensemble2; gm049

    // Amplitude begins at 2323.6, peaks 3680.8 at 0.7s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.1s.
    { 691,692,  0,  40000,  1053 }, // 699: b46M50; b47M50; f20GM50; f36GM50; f48GM50; qGM50; Synth Strings 1; gm050

    // Amplitude begins at  243.7, peaks 1681.2 at 1.8s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    {  50,693,  0,  40000,   206 }, // 700: b46M51; b47M51; f20GM51; f31GM51; f36GM51; f49GM51; qGM51; SynthStrings 2; gm051

    // Amplitude begins at  664.1, peaks 3359.8 at 0.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 694,695,  0,  40000,    73 }, // 701: b46M52; b47M52; f20GM52; f31GM52; f36GM52; f49GM52; qGM52; Choir Aahs; gm052

    // Amplitude begins at  621.8, peaks  941.6 at 0.0s,
    // fades to 20% at 3.6s, keyoff fades to 20% in 3.6s.
    { 696,697,  0,   3646,  3646 }, // 702: b46M53; b47M53; f20GM53; f31GM53; f36GM53; f49GM53; qGM53; Voice Oohs; gm053

    // Amplitude begins at  527.1, peaks 2564.7 at 9.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 698,699,  0,  40000,   193 }, // 703: b46M54; b47M54; f20GM54; f31GM54; f36GM54; f48GM54; f49GM54; qGM54; Synth Voice; gm054

    // Amplitude begins at  492.6, peaks  753.4 at 0.0s,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 700,701,  0,    446,   446 }, // 704: b46M55; b47M55; f20GM55; f31GM55; f36GM55; f49GM55; qGM55; Orchestra Hit; gm055

    // Amplitude begins at  535.3, peaks 2440.1 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    {  55,702,  0,  40000,    40 }, // 705: b46M56; b47M56; f20GM56; f31GM56; f36GM56; f49GM56; qGM56; Trumpet; gm056

    // Amplitude begins at  555.3, peaks 2190.6 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 703,704,  0,  40000,    40 }, // 706: b46M57; b47M57; f20GM57; f31GM57; f36GM57; f49GM57; qGM57; Trombone; gm057

    // Amplitude begins at   43.6, peaks 3911.4 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 705,706,  0,  40000,    20 }, // 707: f20GM58; f31GM58; f36GM58; f49GM58; qGM58; Tuba

    // Amplitude begins at  312.2, peaks 2472.2 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 707,708,  0,  40000,     0 }, // 708: b46M59; b47M59; f20GM59; f31GM59; f36GM59; f49GM59; qGM59; Muted Trumpet; gm059

    // Amplitude begins at    4.9, peaks 4013.5 at 33.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 709,710,  0,  40000,     6 }, // 709: b46M60; b47M60; f20GM60; f36GM60; f49GM60; qGM60; French Horn; gm060

    // Amplitude begins at    8.0, peaks 1895.6 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 711,712,  0,  40000,    20 }, // 710: b46M61; b47M61; f20GM61; f36GM61; f49GM61; qGM61; Brass Section; gm061

    // Amplitude begins at  623.8, peaks 1248.5 at 26.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 713,714,  0,  40000,    20 }, // 711: b46M62; b47M62; f20GM62; f31GM62; f36GM62; f49GM62; qGM62; Synth Brass 1; gm062

    // Amplitude begins at   89.4, peaks 6618.9 at 0.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 715,716,  0,  40000,     6 }, // 712: b46M63; b47M63; f20GM63; f36GM63; f49GM63; qGM63; Synth Brass 2; gm063

    // Amplitude begins at  337.4, peaks 1677.9 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 717,718,  0,  40000,     6 }, // 713: b46M64; b47M64; f20GM64; f31GM64; f36GM64; f49GM64; qGM64; Soprano Sax; gm064

    // Amplitude begins at 2680.4, peaks 3836.9 at 38.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 719,720,  0,  40000,   126 }, // 714: b46M65; b47M65; f20GM65; f31GM65; f36GM65; f49GM65; qGM65; Alto Sax; gm065

    // Amplitude begins at  463.6, peaks 5305.5 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 721,722,  0,  40000,    40 }, // 715: b46M66; b47M66; f20GM66; f31GM66; f36GM66; f49GM66; qGM66; Tenor Sax; gm066

    // Amplitude begins at  957.9, peaks 5365.3 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 723,724,  0,  40000,    93 }, // 716: b46M67; b47M67; f20GM67; f31GM67; f36GM67; f48GM67; f49GM67; qGM67; Baritone Sax; gm067

    // Amplitude begins at 1119.7, peaks 1528.2 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 725,726,  0,  40000,    46 }, // 717: b46M68; b47M68; f20GM68; f36GM68; f49GM68; qGM68; Oboe; gm068

    // Amplitude begins at   25.3, peaks 4103.8 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 727,728,  0,  40000,    73 }, // 718: b46M69; b47M69; f20GM69; f31GM69; f36GM69; f49GM69; qGM69; English Horn; gm069

    // Amplitude begins at  112.1, peaks 3903.7 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 729,730,  0,  40000,     0 }, // 719: b46M70; b47M70; f20GM70; f31GM70; f36GM70; f49GM70; qGM70; Bassoon; gm070

    // Amplitude begins at    3.4, peaks 1731.0 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 731,732,  0,  40000,    60 }, // 720: b46M71; b47M71; f20GM71; f31GM71; f36GM71; f49GM71; qGM71; Clarinet; gm071

    // Amplitude begins at  617.1, peaks 2979.6 at 0.0s,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 733,734,  0,    400,   400 }, // 721: b46M72; b47M72; f20GM72; f31GM72; f36GM72; f49GM72; qGM72; Piccolo; gm072

    // Amplitude begins at  604.7, peaks 1152.6 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 733,735,  0,  40000,     6 }, // 722: b46M73; b47M73; f20GM73; f31GM73; f36GM73; f49GM73; qGM73; Flute; gm073

    // Amplitude begins at    0.0, peaks 1280.1 at 32.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 736,737,  0,  40000,    33 }, // 723: b46M74; b47M74; f20GM74; f36GM74; f48GM74; f49GM74; qGM74; Recorder; gm074

    // Amplitude begins at  632.7, peaks 1184.7 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 738,739,  0,  40000,     6 }, // 724: b46M75; b47M75; f20GM75; f31GM75; f36GM75; qGM75; Pan Flute; gm075

    // Amplitude begins at   31.5, peaks  820.0 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 740,741,  0,  40000,    86 }, // 725: b46M76; b47M76; f20GM76; f31GM76; f36GM76; f48GM76; f49GM76; qGM76; Bottle Blow; gm076

    // Amplitude begins at    2.3, peaks 3151.3 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 742,743,  0,  40000,    26 }, // 726: b46M77; b47M77; f20GM77; f31GM77; f36GM77; f49GM77; qGM77; Shakuhachi; gm077

    // Amplitude begins at    0.6, peaks 3576.6 at 0.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 744,745,  0,  40000,   193 }, // 727: b46M78; b47M78; f20GM78; f31GM78; f36GM78; f48GM78; f49GM78; qGM78; Whistle; gm078

    // Amplitude begins at    3.5, peaks 1352.4 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 746,747,  0,  40000,    53 }, // 728: b46M79; b47M79; f20GM79; f31GM79; f36GM79; f48GM79; qGM79; Ocarina; gm079

    // Amplitude begins at 3050.7, peaks 4161.3 at 15.8s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 748,749,  0,  40000,     0 }, // 729: b46M80; b47M80; f20GM80; f31GM80; f36GM80; f49GM80; qGM80; Lead 1 squareea; gm080

    // Amplitude begins at 2318.4, peaks 3311.5 at 13.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 750,751,  0,  40000,     6 }, // 730: b46M81; b47M81; f20GM81; f31GM81; f36GM81; f49GM81; qGM81; Lead 2 sawtooth; gm081

    // Amplitude begins at  616.9, peaks  945.9 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 752,753,  0,  40000,    26 }, // 731: b46M82; b47M82; f20GM82; f31GM82; f36GM82; f49GM82; qGM82; Lead 3 calliope; gm082

    // Amplitude begins at  392.8, peaks 5203.3 at 0.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.5s.
    { 754,755,  0,  40000,   546 }, // 732: b46M83; b47M83; f20GM83; f31GM83; f36GM83; f48GM83; f49GM83; qGM83; Lead 4 chiff; gm083

    // Amplitude begins at  189.1, peaks 3425.8 at 0.0s,
    // fades to 20% at 2.8s, keyoff fades to 20% in 0.0s.
    { 756,757,  0,   2813,     6 }, // 733: b46M84; b47M84; f20GM84; f31GM84; f36GM84; f48GM84; f49GM84; qGM84; Lead 5 charang; gm084

    // Amplitude begins at 1900.9, peaks 2235.9 at 38.8s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 758,759,  0,  40000,   220 }, // 734: b46M85; b47M85; f20GM85; f31GM85; f36GM85; qGM85; Lead 6 voice; gm085

    // Amplitude begins at 1819.9, peaks 4126.0 at 3.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 760,761,  0,  40000,    46 }, // 735: b46M86; b47M86; f20GM86; f31GM86; f36GM86; qGM86; Lead 7 fifths; gm086

    // Amplitude begins at 3300.9, peaks 4254.8 at 1.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    {  86,762,  0,  40000,    20 }, // 736: b46M87; b47M87; f20GM87; f31GM87; f36GM87; qGM87; Lead 8 brass; gm087

    // Amplitude begins at 2408.7, peaks 4961.6 at 0.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 763,764,  0,  40000,   106 }, // 737: b46M88; b47M88; f20GM88; f36GM88; f48GM88; qGM88; Pad 1 new age; gm088

    // Amplitude begins at 2312.6, peaks 6658.2 at 0.7s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.7s.
    {  88,765,  0,  40000,   733 }, // 738: b46M89; b47M89; f20GM89; f31GM89; f36GM89; f49GM89; qGM89; Pad 2 warm; gm089

    // Amplitude begins at  874.6, peaks 4344.4 at 2.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.3s.
    { 766,767,  0,  40000,   266 }, // 739: b46M90; b47M90; f20GM90; f31GM63; f31GM90; f36GM90; f49GM90; qGM90; Pad 3 polysynth; Synth Brass 2; gm090

    // Amplitude begins at    1.4, peaks 3589.2 at 35.8s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.6s.
    { 768,769,  0,  40000,  1613 }, // 740: b46M91; b47M91; f20GM91; f31GM91; f36GM91; f48GM91; f49GM91; qGM91; Pad 4 choir; gm091

    // Amplitude begins at    0.0, peaks 6038.6 at 1.1s,
    // fades to 20% at 4.2s, keyoff fades to 20% in 0.0s.
    { 770,771,  0,   4200,     6 }, // 741: b46M92; b47M92; f20GM92; f31GM92; f36GM92; f49GM92; qGM92; Pad 5 bowedpad; gm092

    // Amplitude begins at    0.6, peaks 3074.5 at 0.4s,
    // fades to 20% at 2.7s, keyoff fades to 20% in 0.1s.
    { 772,773,  0,   2660,    80 }, // 742: b46M93; b47M93; f20GM93; f31GM93; f36GM93; f49GM93; qGM93; Pad 6 metallic; gm093

    // Amplitude begins at    0.0, peaks 3056.7 at 2.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 774,775,  0,  40000,   160 }, // 743: b46M94; b47M94; f20GM94; f31GM94; f36GM94; f49GM94; qGM94; Pad 7 halo; gm094

    // Amplitude begins at 2050.9, peaks 5452.2 at 0.2s,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.0s.
    { 776,777,  0,    853,    40 }, // 744: b46M95; b47M95; f20GM95; f31GM95; f36GM95; f48GM95; qGM95; Pad 8 sweep; gm095

    // Amplitude begins at 1270.0, peaks 3305.2 at 22.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.9s.
    { 778,779,  0,  40000,   933 }, // 745: b46M96; b47M96; f20GM96; f31GM96; f36GM96; f49GM96; qGM96; FX 1 rain; gm096

    // Amplitude begins at    0.0, peaks 2711.4 at 0.6s,
    // fades to 20% at 0.8s, keyoff fades to 20% in 0.2s.
    { 780,781,  0,    846,   240 }, // 746: b46M97; b47M97; f20GM97; f31GM97; f36GM97; f49GM97; qGM97; FX 2 soundtrack; gm097

    // Amplitude begins at  904.9, peaks 4456.3 at 0.1s,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 782,783,  0,    446,   446 }, // 747: b46M98; b47M98; f20GM98; f31GM98; f36GM98; f48GM98; f49GM98; qGM98; FX 3 crystal; gm098

    // Amplitude begins at  892.4, peaks 1649.3 at 0.5s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 784,785,  0,  40000,   193 }, // 748: b46M99; b47M99; f20GM99; f31GM99; f36GM99; qGM99; FX 4 atmosphere; gm099

    // Amplitude begins at 1961.6, peaks 2942.3 at 0.0s,
    // fades to 20% at 1.5s, keyoff fades to 20% in 1.5s.
    { 786,787,  0,   1520,  1520 }, // 749: b46M100; b47M100; f20GM100; f31GM100; f36GM100; f48GM100; f49GM100; qGM100; FX 5 brightness; gm100

    // Amplitude begins at    0.0, peaks 1884.4 at 2.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.6s.
    { 788,789,  0,  40000,   573 }, // 750: b46M101; b47M101; f20GM101; f31GM101; f36GM101; f49GM101; qGM101; FX 6 goblins; gm101

    // Amplitude begins at  910.1, peaks 4894.8 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 790,791,  0,  40000,   180 }, // 751: b46M102; b47M102; f20GM102; f31GM102; f36GM102; f49GM102; qGM102; FX 7 echoes; gm102

    // Amplitude begins at 3545.0, peaks 4825.5 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.3s.
    { 792,793,  0,  40000,   280 }, // 752: b46M103; b47M103; f20GM103; f31GM103; f36GM103; f48GM103; f49GM103; qGM103; FX 8 sci-fi; gm103

    // Amplitude begins at 2966.4, peaks 4015.4 at 0.3s,
    // fades to 20% at 2.1s, keyoff fades to 20% in 2.1s.
    { 794,795,  0,   2060,  2060 }, // 753: b46M104; b47M104; f20GM104; f31GM104; f36GM104; f48GM104; f49GM104; qGM104; Sitar; gm104

    // Amplitude begins at 2432.7,
    // fades to 20% at 0.8s, keyoff fades to 20% in 0.8s.
    { 796,797,  0,    766,   766 }, // 754: b46M105; b47M105; f20GM105; f31GM105; f36GM105; f48GM105; f49GM105; qGM105; Banjo; gm105

    // Amplitude begins at 2810.8, peaks 3728.2 at 0.0s,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.9s.
    { 798,799,  0,    853,   853 }, // 755: b46M106; b47M106; f20GM106; f31GM106; f36GM106; f49GM106; qGM106; Shamisen; gm106

    // Amplitude begins at 4230.8,
    // fades to 20% at 0.7s, keyoff fades to 20% in 0.7s.
    { 800,801,  0,    706,   706 }, // 756: b46M107; b47M107; f20GM107; f31GM107; f36GM107; qGM107; Koto; gm107

    // Amplitude begins at 2440.6,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 107,802,  0,    206,   206 }, // 757: b46M108; b47M108; f20GM108; f31GM108; f31GM45; f36GM108; f48GM108; f49GM108; qGM108; Kalimba; Pizzicato String; gm108

    // Amplitude begins at    9.6, peaks 3869.8 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 108,803,  0,  40000,     0 }, // 758: b46M109; b47M109; f20GM109; f31GM109; f36GM109; f48GM109; qGM109; Bagpipe; gm109

    // Amplitude begins at  902.3, peaks 2558.2 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 109,804,  0,  40000,   106 }, // 759: b46M110; b47M110; f20GM110; f31GM110; f36GM110; f48GM110; f49GM110; qGM110; Fiddle; gm110

    // Amplitude begins at 2604.4, peaks 4428.1 at 19.8s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 805,806,  0,  40000,   180 }, // 760: b46M111; b47M111; f20GM111; f31GM111; f36GM111; f48GM111; qGM111; Shanai; gm111

    // Amplitude begins at 2977.5,
    // fades to 20% at 0.8s, keyoff fades to 20% in 0.8s.
    { 111,807,  0,    813,   813 }, // 761: b46M112; b47M112; f20GM112; f31GM112; f36GM112; f48GM112; qGM112; Tinkle Bell; gm112

    // Amplitude begins at 2744.3,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 808,809,  0,    126,   126 }, // 762: b46M113; b47M113; f20GM113; f31GM113; f36GM113; f48GM113; qGM113; Agogo Bells; gm113

    // Amplitude begins at 2624.2, peaks 4406.8 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 810,811,  0,  40000,   126 }, // 763: b46M114; b47M114; f20GM114; f31GM114; f36GM114; f48GM114; f49GM114; qGM114; Steel Drums; gm114

    // Amplitude begins at 2351.2,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 812,813,  0,     13,    13 }, // 764: b46M115; b47M115; f20GM115; f31GM115; f36GM115; f49GM115; qGM115; Woodblock; gm115

    // Amplitude begins at 2091.1, peaks 2357.8 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 115,814,  0,    126,   126 }, // 765: b46M116; b47M116; f20GM116; f31GM116; f36GM116; f49GM116; qGM116; Taiko Drum; gm116

    // Amplitude begins at 2400.6,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 815,816,  0,     80,    80 }, // 766: b46M117; b47M117; f20GM117; f31GM117; f36GM117; f49GM117; qGM117; Melodic Tom; gm117

    // Amplitude begins at 1038.6, peaks 1098.6 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 817,818,  0,    213,   213 }, // 767: b46M118; b47M118; f20GM118; f31GM118; f36GM118; f48GM118; f49GM118; qGM118; Synth Drum; gm118

    // Amplitude begins at    0.0, peaks  647.7 at 2.3s,
    // fades to 20% at 2.3s, keyoff fades to 20% in 2.3s.
    { 819,820,  0,   2333,  2333 }, // 768: b46M119; b47M119; f20GM119; f31GM119; f36GM119; qGM119; Reverse Cymbal; gm119

    // Amplitude begins at    0.0, peaks  536.5 at 0.1s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 821,822,  0,    286,   286 }, // 769: b46M120; b47M120; f20GM120; f31GM120; f36GM120; qGM120; Guitar FretNoise; gm120

    // Amplitude begins at    0.0, peaks  492.0 at 0.3s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 823,824,  0,    593,   593 }, // 770: b46M121; b47M121; f20GM121; f31GM121; f36GM121; qGM121; Breath Noise; gm121

    // Amplitude begins at    0.0, peaks  478.3 at 2.3s,
    // fades to 20% at 4.7s, keyoff fades to 20% in 4.7s.
    { 825,824,  0,   4720,  4720 }, // 771: b46M122; b47M122; f20GM122; f31GM122; f36GM122; f49GM122; qGM122; Seashore; gm122

    // Amplitude begins at    0.0, peaks 1857.1 at 0.3s,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 826,827,  0,    446,   446 }, // 772: b46M123; b47M123; f20GM123; f31GM123; f36GM123; f49GM123; qGM123; Bird Tweet; gm123

    // Amplitude begins at 1695.1,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 828,829,  0,    153,   153 }, // 773: f20GM124; f31GM124; f36GM124; qGM124; Telephone

    // Amplitude begins at    0.0, peaks 1278.1 at 1.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 830,831,  0,  40000,    80 }, // 774: f20GM125; f31GM125; f36GM125; f49GM125; qGM125; Helicopter

    // Amplitude begins at    0.0, peaks  470.5 at 2.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 832,824,  0,  40000,   113 }, // 775: b46M126; b47M126; f20GM126; f31GM126; f36GM126; qGM126; Applause/Noise; gm126

    // Amplitude begins at  872.0, peaks 1001.5 at 2.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 833,834,  0,  40000,     0 }, // 776: b46M127; b47M127; f20GM127; f31GM127; f36GM127; f49GM127; qGM127; Gunshot; gm127

    // Amplitude begins at 2531.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 835,836, 18,     33,    33 }, // 777: f20GP37; f31GP37; qGP37; Side Stick

    // Amplitude begins at 1853.6, peaks 1993.4 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 129,837,  0,     66,    66 }, // 778: f20GP38; f31GP38; qGP38; Acoustic Snare

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 838,839,130,      0,     0 }, // 779: f20GP39; f31GP39; qGP39; Hand Clap

    // Amplitude begins at  934.2, peaks 1528.4 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 129,840,  0,     60,    60 }, // 780: f20GP40; qGP40; Electric Snare

    // Amplitude begins at  449.1,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 841,842,  1,     33,    33 }, // 781: f20GP41; f20GP43; f20GP45; f20GP47; f20GP48; f20GP50; f31GP41; f31GP43; f31GP45; f31GP47; f31GP48; f31GP50; qGP41; qGP43; qGP45; qGP47; qGP48; qGP50; High Floor Tom; High Tom; High-Mid Tom; Low Floor Tom; Low Tom; Low-Mid Tom

    // Amplitude begins at  720.7,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 132,843, 12,     33,    33 }, // 782: f20GP42; f31GP42; qGP42; Closed High Hat

    // Amplitude begins at    1.7, peaks  918.2 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 844,845, 12,     66,    66 }, // 783: f20GP44; f31GP44; qGP44; Pedal High Hat

    // Amplitude begins at  641.3,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 134,846, 12,    266,   266 }, // 784: f20GP46; f31GP46; qGP46; Open High Hat

    // Amplitude begins at 1072.2, peaks 1332.9 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 847,848,  1,    193,   193 }, // 785: f20GP49; f31GP49; f36GP57; qGP49; Crash Cymbal 1; Crash Cymbal 2

    // Amplitude begins at 1632.1,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 849,850, 15,    353,   353 }, // 786: f20GP51; f31GP51; f31GP59; qGP51; qGP59; Ride Cymbal 1; Ride Cymbal 2

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 851,852,129,      0,     0 }, // 787: f20GP52; f31GP52; qGP52; Chinese Cymbal

    // Amplitude begins at 1516.1,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 853,854, 15,    273,   273 }, // 788: f20GP53; f31GP53; qGP53; Ride Bell

    // Amplitude begins at 1433.5, peaks 2663.7 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 855,856,  6,    133,   133 }, // 789: f20GP54; f31GP54; qGP54; Tambourine

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 857,858,143,      0,     0 }, // 790: f20GP55; f31GP55; qGP55; Splash Cymbal

    // Amplitude begins at 1820.6,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 859,860, 17,      6,     6 }, // 791: f20GP56; f31GP56; qGP56; Cow Bell

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 861,862,129,      0,     0 }, // 792: f20GP57; f31GP57; qGP57; Crash Cymbal 2

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 863,864,128,      0,     0 }, // 793: f20GP58; f31GP58; qGP58; Vibraslap

    // Amplitude begins at 1170.4,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 865,866,  6,      6,     6 }, // 794: f20GP60; f31GP60; qGP60; High Bongo

    // Amplitude begins at 2056.8,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 867,868,  1,     13,    13 }, // 795: f20GP61; f31GP61; qGP61; Low Bongo

    // Amplitude begins at 1029.2,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 145,869,  1,      6,     6 }, // 796: f20GP62; f31GP62; qGP62; Mute High Conga

    // Amplitude begins at  950.5,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 870,871,  1,      6,     6 }, // 797: f20GP63; f31GP63; qGP63; Open High Conga

    // Amplitude begins at  985.8,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 872,873,  1,      6,     6 }, // 798: f20GP64; f31GP64; qGP64; Low Conga

    // Amplitude begins at  545.2,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 874,875,  1,     20,    20 }, // 799: f20GP65; f31GP65; qGP65; High Timbale

    // Amplitude begins at 1127.5,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 149,876,  0,    106,   106 }, // 800: f20GP66; f31GP66; qGP66; Low Timbale

    // Amplitude begins at 1411.8, peaks 1500.1 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 877,878,  3,    106,   106 }, // 801: f20GP67; f31GP67; qGP67; High Agogo

    // Amplitude begins at  969.4, peaks 1144.6 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 879,880,  3,    140,   140 }, // 802: f20GP68; f31GP68; qGP68; Low Agogo

    // Amplitude begins at    0.4, peaks  489.0 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 881,882, 15,    106,   106 }, // 803: f20GP69; f31GP69; qGP69; Cabasa

    // Amplitude begins at  275.0, peaks  459.8 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 883,884, 15,     20,    20 }, // 804: f20GP70; f31GP70; qGP70; Maracas

    // Amplitude begins at   99.7, peaks 1110.3 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 885,886, 87,    326,   326 }, // 805: f20GP71; f31GP71; qGP71; Short Whistle

    // Amplitude begins at  117.2, peaks 1263.9 at 0.0s,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 887,888, 87,    446,   446 }, // 806: f20GP72; f31GP72; qGP72; Long Whistle

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 889,890,128,      0,     0 }, // 807: f20GP73; f31GP73; qGP73; Short Guiro

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 889,891,128,      0,     0 }, // 808: f20GP74; f31GP74; qGP74; Long Guiro

    // Amplitude begins at 2006.6, peaks 3559.3 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 892,893,  6,     53,    53 }, // 809: f20GP75; f31GP75; qGP75; Claves

    // Amplitude begins at 1701.9,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 159,894,  6,      6,     6 }, // 810: f20GP76; f20GP77; f31GP76; f31GP77; qGP76; qGP77; High Wood Block; Low Wood Block

    // Amplitude begins at    0.0, peaks 2132.1 at 0.1s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 895,896,  1,    146,   146 }, // 811: f20GP78; f31GP78; qGP78; Mute Cuica

    // Amplitude begins at    1.1, peaks 2855.8 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 897,898,  1,     46,    46 }, // 812: f20GP79; f31GP79; qGP79; Open Cuica

    // Amplitude begins at 1974.0,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 899,900, 10,    386,   386 }, // 813: f20GP80; f31GP80; qGP80; Mute Triangle

    // Amplitude begins at 2089.5,
    // fades to 20% at 2.3s, keyoff fades to 20% in 2.3s.
    { 901,902, 10,   2280,  2280 }, // 814: f20GP81; f31GP81; qGP81; Open Triangle

    // Amplitude begins at    0.4, peaks  465.5 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 903,904, 15,     53,    53 }, // 815: f20GP82; f31GP82; qGP82; Shaker

    // Amplitude begins at 2934.5,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 905,906,  5,     33,    33 }, // 816: f20GP83; f31GP83; qGP83; Jingle Bell

    // Amplitude begins at  815.5, peaks 1012.6 at 0.1s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 907,908,  3,    220,   220 }, // 817: f20GP84; f31GP84; f36GP84; qGP84; Bell Tree

    // Amplitude begins at 1070.0, peaks 1767.8 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 909,910,  3,     53,    53 }, // 818: f20GP85; f31GP85; qGP85; Castanets

    // Amplitude begins at 1916.8,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 911,912,  1,     13,    13 }, // 819: f20GP86; f31GP86; qGP86; Mute Surdu

    // Amplitude begins at 3658.7,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 913,914,  1,     13,    13 }, // 820: f20GP87; f31GP87; qGP87; Open Surdu

    // Amplitude begins at 2487.8, peaks 3203.1 at 0.1s,
    // fades to 20% at 1.6s, keyoff fades to 20% in 1.6s.
    { 915,915,  0,   1560,  1560 }, // 821: f17GM0; mGM0; AcouGrandPiano

    // Amplitude begins at 1763.6, peaks 2052.1 at 0.5s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 916,916,  0,  40000,     6 }, // 822: f17GM3; f35GM3; mGM3; Honky-tonkPiano

    // Amplitude begins at 2805.0, peaks 2835.6 at 0.0s,
    // fades to 20% at 1.0s, keyoff fades to 20% in 1.0s.
    { 917,917,  0,   1046,  1046 }, // 823: f17GM8; f35GM8; mGM8; Celesta

    // Amplitude begins at 2978.4, peaks 3030.5 at 0.0s,
    // fades to 20% at 1.9s, keyoff fades to 20% in 1.9s.
    { 918,918,  0,   1886,  1886 }, // 824: f17GM11; mGM11; Vibraphone

    // Amplitude begins at 2060.7, peaks 2301.9 at 0.0s,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    { 919,919,  0,   1200,  1200 }, // 825: f17GM14; f35GM14; mGM14; Tubular Bells

    // Amplitude begins at 1677.3, peaks 2151.7 at 0.0s,
    // fades to 20% at 0.5s, keyoff fades to 20% in 0.5s.
    { 920,920,  0,    480,   480 }, // 826: f17GM15; mGM15; Dulcimer

    // Amplitude begins at 2317.1, peaks 2693.5 at 14.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 921,921,  0,  40000,    40 }, // 827: f17GM16; f29GM46; f30GM46; mGM16; Hammond Organ; Orchestral Harp

    // Amplitude begins at 2304.4, peaks 2619.0 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 922,922,  0,  40000,    20 }, // 828: f17GM17; mGM17; Percussive Organ

    // Amplitude begins at  763.8, peaks 2173.7 at 13.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 923,923,  0,  40000,    20 }, // 829: f17GM18; f29GM10; f30GM10; mGM18; Music box; Rock Organ

    // Amplitude begins at  336.5, peaks 2196.5 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.4s.
    { 924,924,  0,  40000,   413 }, // 830: f17GM19; f29GM12; f29GM13; f29GM14; f30GM12; f30GM13; f30GM14; mGM19; Church Organ; Marimba; Tubular Bells; Xylophone

    // Amplitude begins at  154.1, peaks 4544.5 at 29.7s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.3s.
    { 925,925,  0,  40000,   280 }, // 831: f17GM20; f35GM20; mGM20; Reed Organ

    // Amplitude begins at    0.0, peaks 1007.9 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 926,926,  0,  40000,     6 }, // 832: f15GM15; f17GM21; f26GM15; f29GM15; f30GM15; f35GM21; mGM21; Accordion; Dulcimer

    // Amplitude begins at  182.5, peaks 2999.2 at 39.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 927,927,  0,  40000,    86 }, // 833: f17GM22; f29GM87; f30GM87; mGM22; Harmonica; Lead 8 brass

    // Amplitude begins at    3.2, peaks 3122.1 at 2.8s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 928,928,  0,  40000,    73 }, // 834: f17GM23; f35GM23; mGM23; Tango Accordion

    // Amplitude begins at  955.2, peaks 1149.5 at 0.0s,
    // fades to 20% at 1.1s, keyoff fades to 20% in 1.1s.
    { 929,929,  0,   1106,  1106 }, // 835: f17GM24; f30GM59; mGM24; Acoustic Guitar1; Muted Trumpet

    // Amplitude begins at 1425.6, peaks 2713.1 at 1.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 930,930,  0,  40000,     0 }, // 836: f17GM31; mGM31; Guitar Harmonics

    // Amplitude begins at 2279.9, peaks 2476.2 at 0.0s,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    { 931,931,  0,   1173,  1173 }, // 837: f17GM32; f29GM65; f30GM65; mGM32; Acoustic Bass; Alto Sax

    // Amplitude begins at 1406.5, peaks 2877.3 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 932,932,  0,  40000,     6 }, // 838: f17GM42; f29GM54; f29GM55; f30GM54; f30GM55; mGM42; Cello; Orchestra Hit; Synth Voice

    // Amplitude begins at  845.0, peaks 1921.7 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 933,933,  0,  40000,    66 }, // 839: f17GM48; f30GM50; mGM48; String Ensemble1; Synth Strings 1

    // Amplitude begins at    0.0, peaks 1568.9 at 0.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 934,934,  0,  40000,    60 }, // 840: f17GM49; mGM49; String Ensemble2

    // Amplitude begins at 2329.2, peaks 4198.0 at 0.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.9s.
    { 935,935,  0,  40000,   860 }, // 841: f17GM50; f29GM48; f30GM48; mGM50; String Ensemble1; Synth Strings 1

    // Amplitude begins at    0.0, peaks 1003.3 at 0.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.3s.
    { 936,936,  0,  40000,   260 }, // 842: f17GM51; f29GM49; f30GM49; mGM51; String Ensemble2; SynthStrings 2

    // Amplitude begins at  893.3, peaks 3768.9 at 4.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 937,937,  0,  40000,   133 }, // 843: f17GM52; f29GM34; f29GM35; f30GM33; f30GM34; f30GM35; f35GM52; mGM52; Choir Aahs; Electric Bass 1; Electric Bass 2; Fretless Bass

    // Amplitude begins at    6.6, peaks 2905.2 at 0.3s,
    // fades to 20% at 4.6s, keyoff fades to 20% in 4.6s.
    { 938,938,  0,   4626,  4626 }, // 844: f17GM53; f35GM53; mGM53; Voice Oohs

    // Amplitude begins at   49.0, peaks 3986.6 at 36.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 939,939,  0,  40000,    93 }, // 845: f17GM54; f29GM39; f30GM39; f35GM54; mGM54; Synth Bass 2; Synth Voice

    // Amplitude begins at  896.8, peaks 1548.2 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 940,940,  0,    260,   260 }, // 846: f17GM55; f29GM122; f30GM122; mGM55; Orchestra Hit; Seashore

    // Amplitude begins at   39.3, peaks 1038.1 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 941,941,  0,  40000,    20 }, // 847: f17GM58; f29GM94; f30GM94; mGM58; Pad 7 halo; Tuba

    // Amplitude begins at    4.0, peaks 1721.0 at 2.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 942,942,  0,  40000,     0 }, // 848: f17GM61; mGM61; Brass Section

    // Amplitude begins at    7.2, peaks 3023.0 at 0.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 943,943,  0,  40000,    20 }, // 849: f17GM62; mGM62; Synth Brass 1

    // Amplitude begins at  674.0, peaks 3305.1 at 38.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 944,944,  0,  40000,   126 }, // 850: f17GM64; f35GM64; mGM64; Soprano Sax

    // Amplitude begins at    3.5, peaks 1691.3 at 38.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 719,719,  0,  40000,    13 }, // 851: f17GM65; f35GM65; mGM65; Alto Sax

    // Amplitude begins at  979.2, peaks 2996.3 at 23.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 945,945,  0,  40000,   106 }, // 852: f17GM66; f35GM66; mGM66; Tenor Sax

    // Amplitude begins at    2.9, peaks 1422.3 at 38.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 946,946,  0,  40000,     0 }, // 853: f17GM67; f29GM81; f30GM81; f35GM67; mGM67; Baritone Sax; Lead 2 sawtooth

    // Amplitude begins at    5.6, peaks 2629.6 at 28.8s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 947,947,  0,  40000,     6 }, // 854: f17GM74; f30GM76; mGM74; Bottle Blow; Recorder

    // Amplitude begins at    0.8, peaks 2911.7 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 948,948,  0,  40000,    46 }, // 855: f17GM76; f29GM109; f29GM110; f29GM80; f30GM109; f30GM110; f30GM80; f35GM76; mGM76; Bagpipe; Bottle Blow; Fiddle; Lead 1 squareea

    // Amplitude begins at    7.6, peaks 2983.5 at 0.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 949,949,  0,  40000,    66 }, // 856: f17GM77; f29GM107; f30GM107; mGM77; Koto; Shakuhachi

    // Amplitude begins at    0.0, peaks 2941.2 at 34.8s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 950,950,  0,  40000,    66 }, // 857: f17GM78; f29GM108; f30GM108; mGM78; Kalimba; Whistle

    // Amplitude begins at    5.0, peaks 3464.3 at 12.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 951,951,  0,  40000,    66 }, // 858: f17GM79; f29GM111; f30GM111; f35GM79; mGM79; Ocarina; Shanai

    // Amplitude begins at  871.5, peaks 6144.1 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.3s.
    { 952,952,  0,  40000,   300 }, // 859: f17GM83; f35GM83; mGM83; Lead 4 chiff

    // Amplitude begins at    0.6, peaks 2279.9 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 953,953,  0,  40000,    66 }, // 860: f17GM85; f35GM85; mGM85; Lead 6 voice

    // Amplitude begins at  113.4, peaks 1067.6 at 35.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 954,954,  0,  40000,   126 }, // 861: f17GM86; f35GM86; mGM86; Lead 7 fifths

    // Amplitude begins at 1508.5, peaks 3780.7 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 955,955,  0,  40000,   160 }, // 862: f17GM88; f29GM32; f30GM32; mGM88; Acoustic Bass; Pad 1 new age

    // Amplitude begins at 1578.4, peaks 3276.2 at 10.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 2.7s.
    { 956,956,  0,  40000,  2686 }, // 863: f17GM89; f35GM89; mGM89; Pad 2 warm

    // Amplitude begins at  638.3, peaks 3827.6 at 0.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 957,957,  0,  40000,    73 }, // 864: f17GM90; mGM90; Pad 3 polysynth

    // Amplitude begins at    7.2, peaks 4677.6 at 1.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.4s.
    { 958,958,  0,  40000,  1353 }, // 865: f17GM91; f35GM91; mGM91; Pad 4 choir

    // Amplitude begins at    0.0, peaks 3801.6 at 1.2s,
    // fades to 20% at 4.4s, keyoff fades to 20% in 0.0s.
    { 959,959,  0,   4393,    13 }, // 866: f17GM92; f35GM92; mGM92; Pad 5 bowedpad

    // Amplitude begins at    0.0, peaks 1718.3 at 0.6s,
    // fades to 20% at 2.5s, keyoff fades to 20% in 2.5s.
    { 960,960,  0,   2466,  2466 }, // 867: f17GM93; f35GM93; mGM93; Pad 6 metallic

    // Amplitude begins at    0.0, peaks 2997.7 at 2.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 961,961,  0,  40000,   193 }, // 868: f17GM94; f35GM94; mGM94; Pad 7 halo

    // Amplitude begins at 2050.9, peaks 4271.6 at 2.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 962,962,  0,  40000,    40 }, // 869: f17GM95; f35GM95; mGM95; Pad 8 sweep

    // Amplitude begins at  852.2, peaks 2458.1 at 0.0s,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.9s.
    { 963,963,  0,    906,   906 }, // 870: f17GM96; f29GM41; f29GM43; f30GM41; f30GM43; mGM96; Contrabass; FX 1 rain; Viola

    // Amplitude begins at  992.1, peaks 1022.7 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 964,964,  0,  40000,    93 }, // 871: f17GM99; f29GM37; f30GM37; mGM99; FX 4 atmosphere; Slap Bass 2

    // Amplitude begins at 3085.5, peaks 3423.3 at 0.0s,
    // fades to 20% at 2.1s, keyoff fades to 20% in 2.1s.
    { 965,965,  0,   2060,  2060 }, // 872: f17GM100; f35GM100; mGM100; FX 5 brightness

    // Amplitude begins at    0.0, peaks 1743.8 at 2.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.9s.
    { 966,966,  0,  40000,   886 }, // 873: f17GM101; f35GM101; mGM101; FX 6 goblins

    // Amplitude begins at    0.0, peaks 2012.9 at 4.7s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.9s.
    { 967,967,  0,  40000,   946 }, // 874: f17GM102; f35GM102; mGM102; FX 7 echoes

    // Amplitude begins at   87.0, peaks 2071.0 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.5s.
    { 968,968,  0,  40000,   506 }, // 875: f17GM103; f35GM103; mGM103; FX 8 sci-fi

    // Amplitude begins at 2863.4,
    // fades to 20% at 0.7s, keyoff fades to 20% in 0.7s.
    { 969,969,  0,    706,   706 }, // 876: f17GM107; f29GM105; f30GM105; f35GM107; mGM107; Banjo; Koto

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 970,970,142,      0,     0 }, // 877: f17GP55; f29GP55; f30GP55; f35GP55; f49GP55; mGP55; Splash Cymbal

    // Amplitude begins at  698.6,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 971,971, 64,    220,   220 }, // 878: f17GP58; f29GP58; f30GP58; f35GP58; f49GP58; mGP58; Vibraslap

    // Amplitude begins at    6.1, peaks  445.0 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 972,972, 64,     53,    53 }, // 879: f17GP73; f29GP73; f30GP73; f35GP73; f49GP73; mGP73; Short Guiro

    // Amplitude begins at    0.0, peaks  412.6 at 0.1s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 973,973, 64,    166,   166 }, // 880: f17GP74; f29GP74; f30GP74; f49GP74; mGP74; Long Guiro

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 974,974,129,      0,     0 }, // 881: f17GP79; f29GP79; f30GP79; f35GP79; f48GP79; f49GP79; mGP79; Open Cuica

    // Amplitude begins at 3490.6, peaks 3837.1 at 0.1s,
    // fades to 20% at 3.4s, keyoff fades to 20% in 3.4s.
    { 975,975,  0,   3406,  3406 }, // 882: MGM0; MGM7; f19GM0; f19GM7; f21GM0; f21GM7; f23GM7; f32GM0; f32GM7; f37GM0; f41GM0; f41GM7; f47GM1; AcouGrandPiano; BrightAcouGrand; Clavinet

    // Amplitude begins at 2452.8, peaks 3110.0 at 0.1s,
    // fades to 20% at 2.4s, keyoff fades to 20% in 2.4s.
    { 976,976,  0,   2373,  2373 }, // 883: MGM1; MGM3; MGM5; b41M1; b41M5; f19GM1; f19GM5; f21GM1; f23GM3; f23GM5; f32GM1; f32GM3; f32GM5; f41GM1; f47GM5; BrightAcouGrand; Chorused Piano; Honky-tonkPiano; elpiano1; piano3.i

    // Amplitude begins at 3856.9, peaks 4825.6 at 0.0s,
    // fades to 20% at 3.0s, keyoff fades to 20% in 3.0s.
    { 977,977,  0,   2960,  2960 }, // 884: MGM2; f23GM2; f32GM2; f47GM3; ElecGrandPiano; Honky-tonkPiano

    // Amplitude begins at  855.8,
    // fades to 20% at 0.5s, keyoff fades to 20% in 0.5s.
    { 978,978,  0,    460,   460 }, // 885: MGM4; f23GM4; f32GM4; Rhodes Piano

    // Amplitude begins at 3363.0, peaks 3868.0 at 0.0s,
    // fades to 20% at 3.4s, keyoff fades to 20% in 3.4s.
    { 979,979,  0,   3406,  3406 }, // 886: MGM6; b41M6; f19GM6; f21GM6; f23GM6; f32GM6; Harpsichord; pianof.i

    // Amplitude begins at 2169.1, peaks 2236.4 at 35.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 980,980,  0,  40000,     0 }, // 887: MGM8; f23GM8; f32GM8; f35GM17; Celesta; Percussive Organ

    // Amplitude begins at 2240.2, peaks 2483.5 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 981,981,  0,  40000,   153 }, // 888: MGM10; MGM9; f19GM9; f23GM10; f23GM9; Glockenspiel; Music box

    // Amplitude begins at 2439.6, peaks 3173.6 at 2.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 982,982,  0,  40000,     6 }, // 889: MGM11; f23GM11; f35GM16; oGM11; Hammond Organ; Vibraphone

    // Amplitude begins at 1872.8, peaks 2510.5 at 34.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 983,983,  0,  40000,   153 }, // 890: MGM12; MGM13; MGM14; f23GM12; f23GM13; f23GM14; Marimba; Tubular Bells; Xylophone

    // Amplitude begins at 2946.0, peaks 3521.3 at 10.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 984,984,  0,  40000,     6 }, // 891: MGM15; b41M63; f19GM63; f23GM15; f32GM15; f41GM63; Dulcimer; Synth Brass 2; accordn.

    // Amplitude begins at 1290.2, peaks 1424.7 at 0.0s,
    // fades to 20% at 1.6s, keyoff fades to 20% in 1.6s.
    { 985,985,  0,   1586,  1586 }, // 892: MGM16; MGM17; MGM18; b41M18; f19GM16; f19GM17; f19GM18; f21GM18; f23GM16; f23GM17; f23GM18; f32GM16; f32GM17; f32GM18; f41GM18; Hammond Organ; Percussive Organ; Rock Organ; harpsi4.

    // Amplitude begins at 1803.4, peaks 1912.9 at 0.1s,
    // fades to 20% at 2.0s, keyoff fades to 20% in 2.0s.
    { 986,986,  0,   1973,  1973 }, // 893: MGM19; MGM20; MGM21; b41M20; f12GM7; f16GM7; f19GM20; f21GM20; f23GM19; f23GM20; f23GM21; f32GM19; f32GM20; f32GM21; f41GM20; f54GM7; Accordion; Church Organ; Clavinet; Reed Organ; elclav2.

    // Amplitude begins at   66.3, peaks 1185.2 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 987,987,  0,    300,   300 }, // 894: MGM22; MGM23; f19GM22; f19GM23; f23GM22; f23GM23; f32GM22; f32GM23; Harmonica; Tango Accordion

    // Amplitude begins at   47.0, peaks 1224.0 at 0.0s,
    // fades to 20% at 4.4s, keyoff fades to 20% in 4.4s.
    { 988,988,  0,   4366,  4366 }, // 895: MGM24; MGM25; MGM26; MGM27; f19GM27; f32GM24; f32GM25; f32GM26; f32GM27; oGM26; Acoustic Guitar1; Acoustic Guitar2; Electric Guitar1; Electric Guitar2

    // Amplitude begins at 3708.6, peaks 4065.1 at 0.0s,
    // fades to 20% at 1.6s, keyoff fades to 20% in 1.6s.
    { 989,989,  0,   1560,  1560 }, // 896: MGM28; MGM29; MGM30; MGM31; MGM44; MGM45; MGM65; MGM66; MGM67; b41M29; b41M30; b41M31; b41M44; b41M45; b41M65; b41M66; f15GM65; f19GM28; f19GM29; f19GM30; f19GM31; f19GM44; f19GM45; f19GM65; f19GM66; f19GM67; f21GM30; f21GM31; f23GM28; f23GM29; f23GM31; f23GM44; f23GM45; f26GM65; f32GM28; f32GM29; f32GM30; f32GM31; f32GM44; f32GM45; f32GM65; f32GM66; f32GM67; f41GM29; f41GM30; f41GM31; f41GM44; f41GM45; f41GM65; f41GM66; f41GM67; f47GM34; oGM28; oGM29; oGM30; oGM31; oGM44; oGM45; oGM65; oGM66; oGM67; Alto Sax; Baritone Sax; Distorton Guitar; Electric Bass 2; Electric Guitar3; Guitar Harmonics; Overdrive Guitar; Pizzicato String; Tenor Sax; Tremulo Strings; bass2.in

    // Amplitude begins at 1726.9, peaks 2508.8 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.1s.
    { 990,990,  0,  40000,  1073 }, // 897: MGM32; f23GM32; Acoustic Bass

    // Amplitude begins at    0.0, peaks 3087.2 at 0.5s,
    // fades to 20% at 4.9s, keyoff fades to 20% in 4.9s.
    { 991,991,  0,   4940,  4940 }, // 898: MGM125; MGM33; MGM36; f23GM33; f23GM36; f32GM125; f32GM33; f32GM36; f53GM125; Electric Bass 1; Helicopter; Slap Bass 1

    // Amplitude begins at    0.0, peaks 2866.8 at 0.5s,
    // fades to 20% at 1.8s, keyoff fades to 20% in 1.8s.
    { 992,992,  0,   1766,  1766 }, // 899: MGM34; f19GM34; f23GM34; Electric Bass 2

    // Amplitude begins at 2428.0, peaks 3095.4 at 0.0s,
    // fades to 20% at 4.1s, keyoff fades to 20% in 4.1s.
    { 993,993,  0,   4093,  4093 }, // 900: MGM35; f19GM35; f23GM35; Fretless Bass

    // Amplitude begins at 1541.1, peaks 2166.3 at 0.0s,
    // fades to 20% at 1.1s, keyoff fades to 20% in 1.1s.
    { 295,295,  0,   1066,  1066 }, // 901: MGM38; f23GM38; f32GM38; Synth Bass 1

    // Amplitude begins at    2.4, peaks 1006.7 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 994,994,  0,  40000,     0 }, // 902: MGM39; f19GM39; f23GM39; oGM39; Synth Bass 2

    // Amplitude begins at 3212.7, peaks 3424.1 at 0.0s,
    // fades to 20% at 2.2s, keyoff fades to 20% in 2.2s.
    { 995,995,  0,   2173,  2173 }, // 903: MGM126; MGM40; MGM41; b41M40; f19GM40; f23GM126; f23GM40; f23GM41; f32GM126; f32GM40; f32GM41; f35GM112; f41GM40; Applause/Noise; Tinkle Bell; Viola; Violin; bells.in

    // Amplitude begins at  113.7, peaks 3474.9 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 996,996,  0,  40000,     0 }, // 904: MGM42; f19GM42; f23GM42; oGM42; Cello

    // Amplitude begins at    0.3, peaks 2860.8 at 1.1s,
    // fades to 20% at 1.3s, keyoff fades to 20% in 1.3s.
    { 997,997,  0,   1300,  1300 }, // 905: MGM43; MGM70; MGM71; b41M70; f19GM70; f23GM43; f23GM70; f32GM43; f32GM70; f32GM71; f41GM70; Bassoon; Clarinet; Contrabass; bass1.in

    // Amplitude begins at  433.0, peaks 2886.3 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 998,998,  0,    186,   186 }, // 906: MGM46; f19GM46; f23GM46; oGM46; Orchestral Harp

    // Amplitude begins at 1717.1, peaks 1784.1 at 0.0s,
    // fades to 20% at 2.6s, keyoff fades to 20% in 2.6s.
    { 999,999,  0,   2580,  2580 }, // 907: MGM47; f19GM47; f23GM47; f32GM47; Timpany

    // Amplitude begins at   73.6, peaks 2454.9 at 0.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.1s.
    { 1000,1000,  0,  40000,  1120 }, // 908: MGM48; MGM50; String Ensemble1; Synth Strings 1

    // Amplitude begins at 1287.6, peaks 1447.4 at 0.0s,
    // fades to 20% at 4.4s, keyoff fades to 20% in 4.4s.
    { 1001,1001,  0,   4386,  4386 }, // 909: MGM49; f23GM49; f32GM49; String Ensemble2

    // Amplitude begins at  796.4,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1002,1002,  0,     80,    80 }, // 910: MGM105; MGM51; f32GM105; f32GM51; oGM105; Banjo; SynthStrings 2

    // Amplitude begins at    0.0, peaks  930.5 at 0.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.7s.
    { 1003,1003,  0,  40000,   673 }, // 911: MGM52; MGM54; f19GM52; f19GM54; f23GM52; f23GM54; oGM52; oGM54; Choir Aahs; Synth Voice

    // Amplitude begins at    0.0, peaks  785.7 at 0.1s,
    // fades to 20% at 0.8s, keyoff fades to 20% in 0.8s.
    { 1004,1004,  0,    773,   773 }, // 912: MGM53; f19GM53; f23GM53; f35GM40; Violin; Voice Oohs

    // Amplitude begins at    0.3, peaks 4488.6 at 0.3s,
    // fades to 20% at 1.1s, keyoff fades to 20% in 0.0s.
    { 1005,1005,  0,   1146,    13 }, // 913: MGM55; f19GM55; f23GM55; Orchestra Hit

    // Amplitude begins at  128.6, peaks 2935.1 at 0.0s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 1006,1006,  0,    620,   620 }, // 914: MGM56; b41M56; f19GM56; f21GM56; f23GM56; f32GM56; f41GM56; Trumpet; contrab.

    // Amplitude begins at 2532.5, peaks 3938.3 at 0.0s,
    // fades to 20% at 1.5s, keyoff fades to 20% in 1.5s.
    { 1007,1007,  0,   1466,  1466 }, // 915: MGM57; b41M57; f19GM57; f21GM57; f23GM57; f25GM57; f32GM57; f41GM57; f47GM46; Orchestral Harp; Trombone; harp1.in

    // Amplitude begins at 1166.3, peaks 1633.6 at 0.1s,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.9s.
    { 1008,1008,  0,    940,   940 }, // 916: MGM58; b41M58; f19GM58; f21GM58; f23GM58; f32GM58; f41GM58; Tuba; harp.ins

    // Amplitude begins at 3466.4, peaks 4391.9 at 0.0s,
    // fades to 20% at 0.5s, keyoff fades to 20% in 0.5s.
    { 1009,1009,  0,    526,   526 }, // 917: MGM59; MGM60; b41M59; b41M60; f19GM59; f19GM60; f21GM59; f21GM60; f23GM59; f23GM60; f32GM59; f32GM60; f41GM59; French Horn; Muted Trumpet; guitar1.

    // Amplitude begins at  996.0,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 1010,1010,  0,    606,   606 }, // 918: MGM61; f12GM27; f16GM27; f19GM61; f23GM61; f32GM61; f54GM27; Brass Section; Electric Guitar2

    // Amplitude begins at 1747.2, peaks 2389.4 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1011,1011,  0,  40000,    20 }, // 919: MGM62; f19GM62; f23GM62; f32GM62; Synth Brass 1

    // Amplitude begins at 2699.2, peaks 2820.5 at 0.4s,
    // fades to 20% at 4.9s, keyoff fades to 20% in 4.9s.
    { 1012,1012,  0,   4940,  4940 }, // 920: MGM63; f23GM63; f32GM63; Synth Brass 2

    // Amplitude begins at  874.8, peaks 1363.4 at 0.0s,
    // fades to 20% at 1.9s, keyoff fades to 20% in 1.9s.
    { 1013,1013,  0,   1866,  1866 }, // 921: MGM64; MGM68; MGM69; b41M64; b41M68; b41M69; f19GM64; f19GM68; f19GM69; f32GM64; f32GM68; f32GM69; f41GM64; f41GM68; f41GM69; oGM68; oGM69; English Horn; Oboe; Soprano Sax; bbass.in

    // Amplitude begins at    6.3, peaks 5038.4 at 0.1s,
    // fades to 20% at 1.0s, keyoff fades to 20% in 1.0s.
    { 1014,1014,  0,    960,   960 }, // 922: MGM72; MGM73; MGM74; MGM75; f35GM73; Flute; Pan Flute; Piccolo; Recorder

    // Amplitude begins at    0.0, peaks  763.3 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1015,1015,  0,  40000,     6 }, // 923: MGM110; MGM111; MGM76; MGM77; f19GM110; f19GM111; f35GM74; oGM110; oGM111; oGM77; Bottle Blow; Fiddle; Recorder; Shakuhachi; Shanai

    // Amplitude begins at    6.6, peaks 3450.9 at 0.1s,
    // fades to 20% at 3.9s, keyoff fades to 20% in 3.9s.
    { 1016,1016,  0,   3860,  3860 }, // 924: MGM78; MGM79; MGM80; b41M79; b41M80; f19GM78; f19GM79; f19GM80; f23GM79; f32GM78; f32GM79; f32GM80; Lead 1 squareea; Ocarina; Whistle; sax1.ins

    // Amplitude begins at    0.0, peaks  777.4 at 0.1s,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    { 1017,1017,  0,   1206,  1206 }, // 925: MGM81; Lead 2 sawtooth

    // Amplitude begins at    1.5, peaks  890.5 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 1018,1018,  0,    346,   346 }, // 926: MGM82; f32GM82; Lead 3 calliope

    // Amplitude begins at  633.4, peaks 1127.1 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1019,1019,  0,  40000,     0 }, // 927: MGM83; f19GM83; f35GM71; Clarinet; Lead 4 chiff

    // Amplitude begins at  113.3, peaks 3445.0 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1020,1020,  0,  40000,     0 }, // 928: MGM84; MGM85; f19GM84; f19GM85; Lead 5 charang; Lead 6 voice

    // Amplitude begins at 2049.5, peaks 2054.2 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1021,1021,  0,  40000,     6 }, // 929: MGM86; f19GM86; f35GM70; Bassoon; Lead 7 fifths

    // Amplitude begins at    6.2, peaks 2099.7 at 12.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.5s.
    { 1022,1022,  0,  40000,   493 }, // 930: MGM87; f23GM87; f32GM87; f35GM22; oGM87; Harmonica; Lead 8 brass

    // Amplitude begins at  306.6, peaks 1034.4 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1023,1023,  0,  40000,     0 }, // 931: MGM88; MGM89; f19GM88; f19GM89; f23GM89; f35GM56; Pad 1 new age; Pad 2 warm; Trumpet

    // Amplitude begins at    0.0, peaks  989.5 at 0.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1024,1024,  0,  40000,     0 }, // 932: MGM90; f19GM90; f23GM90; Pad 3 polysynth

    // Amplitude begins at  257.8, peaks  893.9 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1025,1025,  0,  40000,     0 }, // 933: MGM91; f19GM91; f25GM90; f35GM57; Pad 3 polysynth; Pad 4 choir; Trombone

    // Amplitude begins at  559.2, peaks 2847.3 at 0.2s,
    // fades to 20% at 1.3s, keyoff fades to 20% in 0.0s.
    { 1026,1026,  0,   1346,     6 }, // 934: MGM92; b41M92; f19GM92; f25GM92; f32GM92; f41GM92; f47GM60; f53GM93; French Horn; Pad 5 bowedpad; Pad 6 metallic; frhorn1.

    // Amplitude begins at    0.0, peaks 1655.3 at 0.6s,
    // fades to 20% at 0.7s, keyoff fades to 20% in 0.7s.
    { 1027,1027,  0,    706,   706 }, // 935: MGM93; f32GM93; Pad 6 metallic

    // Amplitude begins at   43.0, peaks 3082.4 at 0.3s,
    // fades to 20% at 1.8s, keyoff fades to 20% in 1.8s.
    { 1028,1028,  0,   1800,  1800 }, // 936: MGM94; f32GM94; Pad 7 halo

    // Amplitude begins at   33.0, peaks  892.4 at 0.0s,
    // fades to 20% at 4.5s, keyoff fades to 20% in 4.5s.
    { 1029,1029,  0,   4540,  4540 }, // 937: MGM95; f12GM63; f16GM63; f19GM95; f32GM95; f47GM62; f54GM63; oGM95; Pad 8 sweep; Synth Brass 1; Synth Brass 2

    // Amplitude begins at   40.6, peaks  870.6 at 0.1s,
    // fades to 20% at 7.1s, keyoff fades to 20% in 7.1s.
    { 1030,1030,  0,   7100,  7100 }, // 938: MGM96; f23GM96; oGM96; FX 1 rain

    // Amplitude begins at 3351.9, peaks 3641.0 at 0.0s,
    // fades to 20% at 1.6s, keyoff fades to 20% in 1.6s.
    { 1031,1031,  0,   1606,  1606 }, // 939: MGM97; FX 2 soundtrack

    // Amplitude begins at 1221.6,
    // fades to 20% at 1.1s, keyoff fades to 20% in 1.1s.
    { 1032,1032,  0,   1080,  1080 }, // 940: MGM98; f19GM98; FX 3 crystal

    // Amplitude begins at  969.7,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 1033,1033,  0,    286,   286 }, // 941: MGM104; MGM99; b41M104; b41M99; f19GM104; f19GM99; f32GM104; f32GM99; oGM104; oGM99; FX 4 atmosphere; Sitar; marimba.

    // Amplitude begins at 2290.2,
    // fades to 20% at 40.0s, keyoff fades to 20% in 2.4s.
    { 1034,1034,  0,  40000,  2433 }, // 942: MGM100; MGM101; MGM102; f19GM101; f19GM102; f23GM102; oGM100; oGM101; oGM102; FX 5 brightness; FX 6 goblins; FX 7 echoes

    // Amplitude begins at 2511.3,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 298,298,  0,     46,    46 }, // 943: MGM103; f32GM103; oGM103; FX 8 sci-fi

    // Amplitude begins at   12.4, peaks  523.1 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1035,1035,  0,  40000,     0 }, // 944: MGM106; f15GM106; f19GM106; f26GM106; oGM106; Shamisen

    // Amplitude begins at    1.8, peaks 1939.6 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1036,1036,  0,  40000,    46 }, // 945: MGM107; MGM108; MGM109; oGM108; oGM109; Bagpipe; Kalimba; Koto

    // Amplitude begins at 2505.8,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1037,1037,  0,     40,    40 }, // 946: MGM112; b41M112; f19GM112; f21GM112; f32GM112; f41GM112; Tinkle Bell; bdrum3.i

    // Amplitude begins at  968.1,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 269,269,  0,     13,    13 }, // 947: MGM113; f32GM113; Agogo Bells

    // Amplitude begins at  826.3,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 275,275,  0,    153,   153 }, // 948: MGM114; MGM117; MGM118; b41M117; b41M118; f19GM114; f19GM117; f19GM118; f21GM117; f21GM118; f32GM114; f32GM117; f32GM118; f35GM118; f41GM114; f41GM117; f41GM118; f53GM114; oGM118; Melodic Tom; Steel Drums; Synth Drum; synsnr2.

    // Amplitude begins at  748.3,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 276,276,  0,     40,    40 }, // 949: MGM115; MGM116; b41M116; f19GM116; f21GM116; f32GM115; f32GM116; f41GM116; f53GM115; Taiko Drum; Woodblock; synsnr1.

    // Amplitude begins at  191.7, peaks  439.6 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1038,1038,  0,     86,    86 }, // 950: MGM119; f19GM119; Reverse Cymbal

    // Amplitude begins at 1809.4,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1039,1039,  0,     40,    40 }, // 951: MGM120; f19GM120; Guitar FretNoise

    // Amplitude begins at 2887.7, peaks 2954.7 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1040,1040,  0,  40000,     0 }, // 952: MGM121; f19GM121; f32GM121; Breath Noise

    // Amplitude begins at    0.8, peaks 3183.7 at 0.1s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 1041,1041,  0,    626,   626 }, // 953: MGM122; f19GM122; Seashore

    // Amplitude begins at 1541.1, peaks 2190.1 at 0.0s,
    // fades to 20% at 1.8s, keyoff fades to 20% in 1.8s.
    { 1042,1042,  0,   1833,  1833 }, // 954: MGM123; f19GM123; f21GM123; f32GM123; f41GM123; oGM123; Bird Tweet

    // Amplitude begins at    0.0, peaks  808.9 at 0.1s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1043,1043,  0,    146,   146 }, // 955: MGM124; f23GM124; f32GM124; f35GM123; Bird Tweet; Telephone

    // Amplitude begins at  810.9,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1044,1044,  0,     80,    80 }, // 956: MGM127; oGM127; Gunshot

    // Amplitude begins at 1750.0, peaks 2606.4 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1045,1045,  0,     40,    40 }, // 957: MGP35; MGP36; f32GP35; f32GP36; Ac Bass Drum; Bass Drum 1

    // Amplitude begins at 1031.7,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 269,269,  1,      6,     6 }, // 958: MGP37; MGP41; MGP43; MGP45; MGP47; MGP48; MGP50; MGP56; MGP60; MGP61; MGP62; MGP63; MGP64; MGP65; MGP66; f19GP37; f19GP40; f19GP41; f19GP43; f19GP45; f19GP50; f19GP56; f19GP61; f19GP62; f19GP64; f19GP65; f19GP66; f21GP41; f21GP43; f21GP45; f21GP50; f21GP56; f21GP61; f21GP62; f21GP64; f21GP65; f21GP66; f23GP41; f23GP43; f23GP45; f23GP47; f23GP48; f23GP50; f23GP60; f23GP61; f23GP62; f23GP63; f23GP64; f23GP65; f23GP66; f32GP37; f32GP41; f32GP43; f32GP45; f32GP47; f32GP48; f32GP50; f32GP56; f32GP60; f32GP61; f32GP62; f32GP63; f32GP64; f32GP65; f32GP66; f41GP40; f41GP41; f41GP43; f41GP45; f41GP50; f41GP56; f41GP61; f41GP62; f41GP64; f41GP65; f41GP66; Cow Bell; Electric Snare; High Bongo; High Floor Tom; High Timbale; High Tom; High-Mid Tom; Low Bongo; Low Conga; Low Floor Tom; Low Timbale; Low Tom; Low-Mid Tom; Mute High Conga; Open High Conga; Side Stick

    // Amplitude begins at  825.7,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1046,1046, 46,    133,   133 }, // 959: MGP38; MGP39; MGP40; MGP67; MGP68; f19GP39; f32GP38; f32GP39; f32GP40; f32GP67; f32GP68; f41GP39; Acoustic Snare; Electric Snare; Hand Clap; High Agogo; Low Agogo

    // Amplitude begins at  293.8, peaks  397.8 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1047,1047,100,     40,    40 }, // 960: MGP42; Closed High Hat

    // Amplitude begins at  295.7, peaks  429.0 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1048,1048,100,     66,    66 }, // 961: MGP44; MGP46; MGP51; MGP54; MGP69; MGP70; MGP71; MGP72; MGP73; MGP75; f19GP44; f19GP46; f19GP47; f19GP69; f19GP70; f19GP71; f19GP72; f19GP73; f19GP75; f23GP44; f23GP46; f23GP69; f23GP71; f23GP72; f23GP73; f23GP75; Cabasa; Claves; Long Whistle; Low-Mid Tom; Maracas; Open High Hat; Pedal High Hat; Ride Cymbal 1; Short Guiro; Short Whistle; Tambourine

    // Amplitude begins at  333.2, peaks  425.9 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1049,1049,100,    126,   126 }, // 962: MGP49; Crash Cymbal 1

    // Amplitude begins at 4040.9,
    // fades to 20% at 3.0s, keyoff fades to 20% in 3.0s.
    { 1050,1050,  0,   3006,  3006 }, // 963: oGM0; oGM1; oGM2; AcouGrandPiano; BrightAcouGrand; ElecGrandPiano

    // Amplitude begins at    0.0, peaks  895.7 at 2.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1051,1051,  0,  40000,    60 }, // 964: oGM3; Honky-tonkPiano

    // Amplitude begins at    0.0, peaks 4135.7 at 2.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 2.2s.
    { 1052,1052,  0,  40000,  2200 }, // 965: oGM4; Rhodes Piano

    // Amplitude begins at 3070.8, peaks 4485.8 at 0.0s,
    // fades to 20% at 3.2s, keyoff fades to 20% in 3.2s.
    { 1053,1053,  0,   3166,  3166 }, // 966: oGM5; Chorused Piano

    // Amplitude begins at 1325.7,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1054,1054,  0,    126,   126 }, // 967: oGM6; Harpsichord

    // Amplitude begins at    5.3, peaks 2690.6 at 32.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1055,1055,  0,  40000,     0 }, // 968: oGM7; Clavinet

    // Amplitude begins at 2792.5, peaks 3282.2 at 32.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1056,1056,  0,  40000,    26 }, // 969: oGM8; Celesta

    // Amplitude begins at 1160.1, peaks 1268.1 at 16.8s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1057,1057,  0,  40000,    73 }, // 970: oGM9; Glockenspiel

    // Amplitude begins at 2922.9, peaks 3164.1 at 4.5s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.6s.
    { 1058,1058,  0,  40000,   553 }, // 971: oGM10; Music box

    // Amplitude begins at    0.0, peaks 2138.9 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.5s.
    { 1059,1059,  0,  40000,   473 }, // 972: oGM12; Marimba

    // Amplitude begins at    0.0, peaks 2526.3 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.3s.
    { 1060,1060,  0,  40000,   266 }, // 973: oGM13; Xylophone

    // Amplitude begins at 2887.9, peaks 2895.3 at 0.0s,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.9s.
    { 1061,1061,  0,    906,   906 }, // 974: oGM14; Tubular Bells

    // Amplitude begins at 2472.7, peaks 3115.9 at 0.0s,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    { 1062,1062,  0,   1153,  1153 }, // 975: oGM15; Dulcimer

    // Amplitude begins at 1406.2,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    { 1063,1063,  0,   1200,  1200 }, // 976: f15GM16; f15GM17; f15GM18; f26GM16; f26GM17; f26GM18; oGM16; oGM17; oGM18; Hammond Organ; Percussive Organ; Rock Organ

    // Amplitude begins at 2459.3, peaks 4228.8 at 0.0s,
    // fades to 20% at 0.8s, keyoff fades to 20% in 0.8s.
    { 1064,1064,  0,    833,   833 }, // 977: f15GM19; f15GM20; f15GM21; f26GM19; f26GM20; f26GM21; oGM19; oGM20; oGM21; Accordion; Church Organ; Reed Organ

    // Amplitude begins at 3987.9,
    // fades to 20% at 1.9s, keyoff fades to 20% in 1.9s.
    { 1065,1065,  0,   1913,  1913 }, // 978: f15GM22; f26GM22; oGM22; Harmonica

    // Amplitude begins at 3989.7,
    // fades to 20% at 1.9s, keyoff fades to 20% in 1.9s.
    { 1066,1066,  0,   1913,  1913 }, // 979: f15GM23; f26GM23; oGM23; Tango Accordion

    // Amplitude begins at 1111.7, peaks 1175.7 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1067,1067,  0,  40000,     0 }, // 980: oGM24; Acoustic Guitar1

    // Amplitude begins at   61.0, peaks 2410.2 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1068,1068,  0,  40000,     0 }, // 981: oGM25; Acoustic Guitar2

    // Amplitude begins at  257.8, peaks 4199.6 at 39.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.8s.
    { 1069,1069,  0,  40000,   793 }, // 982: oGM27; Electric Guitar2

    // Amplitude begins at   62.2, peaks 1400.8 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 1070,1070,  0,  40000,   200 }, // 983: oGM32; Acoustic Bass

    // Amplitude begins at    0.0, peaks 5876.2 at 39.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.6s.
    { 1071,1071,  0,  40000,   580 }, // 984: f15GM33; f26GM33; oGM33; Electric Bass 1

    // Amplitude begins at    0.0, peaks 4171.5 at 20.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.7s.
    { 1072,1072,  0,  40000,   746 }, // 985: f15GM34; f26GM34; oGM34; Electric Bass 2

    // Amplitude begins at    1.9, peaks  387.0 at 0.0s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 1073,1073,  0,    640,   640 }, // 986: oGM35; Fretless Bass

    // Amplitude begins at    0.0, peaks 2349.8 at 1.1s,
    // fades to 20% at 2.4s, keyoff fades to 20% in 2.4s.
    { 1074,1074,  0,   2406,  2406 }, // 987: f15GM36; f26GM36; oGM36; Slap Bass 1

    // Amplitude begins at 3212.7, peaks 3495.3 at 0.0s,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    { 1075,1075,  0,   1160,  1160 }, // 988: f15GM38; f26GM38; oGM38; Synth Bass 1

    // Amplitude begins at  122.7, peaks 2711.9 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 2.0s.
    { 1076,1076,  0,  40000,  2013 }, // 989: f15GM40; f26GM40; oGM40; Violin

    // Amplitude begins at 2612.1, peaks 2672.8 at 0.5s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1077,1077,  0,  40000,     0 }, // 990: oGM41; Viola

    // Amplitude begins at 1332.8, peaks 1638.2 at 0.0s,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.9s.
    { 1078,1078,  0,    933,   933 }, // 991: f15GM43; f26GM43; oGM43; Contrabass

    // Amplitude begins at 2354.7, peaks 2459.2 at 0.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1079,1079,  0,  40000,     0 }, // 992: f15GM47; f26GM47; oGM47; Timpany

    // Amplitude begins at    0.4, peaks 2615.3 at 0.7s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1080,1080,  0,  40000,   146 }, // 993: oGM48; String Ensemble1

    // Amplitude begins at    0.6, peaks 1949.8 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1081,1081,  0,  40000,   126 }, // 994: f15GM49; f26GM49; oGM49; String Ensemble2

    // Amplitude begins at 1574.4, peaks 4048.7 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1082,1082,  0,  40000,   106 }, // 995: oGM50; Synth Strings 1

    // Amplitude begins at 2765.2, peaks 2941.3 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 1083,1083,  0,    313,   313 }, // 996: f15GM51; f26GM51; oGM51; SynthStrings 2

    // Amplitude begins at    0.0, peaks  800.4 at 0.1s,
    // fades to 20% at 0.7s, keyoff fades to 20% in 0.7s.
    { 1084,1084,  0,    660,   660 }, // 997: oGM53; Voice Oohs

    // Amplitude begins at    0.3, peaks 4368.1 at 0.3s,
    // fades to 20% at 1.1s, keyoff fades to 20% in 0.0s.
    { 1085,1085,  0,   1086,    13 }, // 998: oGM55; Orchestra Hit

    // Amplitude begins at   84.8, peaks 2743.2 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1086,1086,  0,  40000,   133 }, // 999: oGM56; Trumpet

    // Amplitude begins at 1246.0, peaks 1409.7 at 0.0s,
    // fades to 20% at 2.4s, keyoff fades to 20% in 2.4s.
    { 1087,1087,  0,   2373,  2373 }, // 1000: oGM59; Muted Trumpet

    // Amplitude begins at 2607.3, peaks 2633.3 at 0.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1088,1088,  0,  40000,     0 }, // 1001: oGM60; French Horn

    // Amplitude begins at 2328.2, peaks 2462.5 at 28.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1089,1089,  0,  40000,     0 }, // 1002: oGM61; Brass Section

    // Amplitude begins at  573.8, peaks 1746.2 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1090,1090,  0,  40000,    13 }, // 1003: oGM62; Synth Brass 1

    // Amplitude begins at    0.4, peaks 1445.8 at 0.1s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 1091,1091,  0,    186,   186 }, // 1004: oGM63; Synth Brass 2

    // Amplitude begins at 2497.6, peaks 4330.5 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1092,1092,  0,  40000,     0 }, // 1005: f15GM64; f26GM64; oGM64; Soprano Sax

    // Amplitude begins at 1037.1, peaks 4206.7 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1093,1093,  0,  40000,     0 }, // 1006: f15GM70; f15GM71; f26GM70; f26GM71; oGM70; oGM71; Bassoon; Clarinet

    // Amplitude begins at    0.5, peaks 2356.5 at 0.1s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 1094,1094,  0,    220,   220 }, // 1007: f15GM72; f26GM72; oGM72; Piccolo

    // Amplitude begins at    6.1, peaks 2349.6 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.0s.
    { 1095,1095,  0,    273,     6 }, // 1008: f15GM73; f15GM74; f26GM73; f26GM74; oGM73; oGM74; Flute; Recorder

    // Amplitude begins at    5.3, peaks 2451.4 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1096,1096,  0,  40000,     6 }, // 1009: f15GM75; f26GM75; oGM75; Pan Flute

    // Amplitude begins at   13.1, peaks 5716.4 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1097,1097,  0,  40000,     0 }, // 1010: f15GM76; f26GM76; oGM76; Bottle Blow

    // Amplitude begins at  894.7, peaks 2825.3 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1098,1098,  0,  40000,     0 }, // 1011: oGM78; Whistle

    // Amplitude begins at    6.1, peaks 2314.2 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.3s.
    { 1099,1099,  0,  40000,   286 }, // 1012: oGM79; Ocarina

    // Amplitude begins at 1391.8, peaks 1475.9 at 0.0s,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    { 1100,1100,  0,   1213,  1213 }, // 1013: oGM80; Lead 1 squareea

    // Amplitude begins at  782.2, peaks 2219.2 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.3s.
    { 1101,1101,  0,  40000,   286 }, // 1014: oGM81; Lead 2 sawtooth

    // Amplitude begins at  136.9, peaks 3849.3 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.9s.
    { 1102,1102,  0,  40000,   886 }, // 1015: f15GM82; f26GM82; oGM82; Lead 3 calliope

    // Amplitude begins at 1846.7, peaks 3840.6 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.9s.
    { 1103,1103,  0,  40000,   886 }, // 1016: f15GM83; f26GM83; oGM83; Lead 4 chiff

    // Amplitude begins at    0.8, peaks 3267.0 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.3s.
    { 1104,1104,  0,  40000,  1280 }, // 1017: f15GM84; f26GM84; oGM84; Lead 5 charang

    // Amplitude begins at   21.7, peaks 2598.8 at 5.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1105,1105,  0,  40000,    20 }, // 1018: f15GM85; f26GM85; oGM85; Lead 6 voice

    // Amplitude begins at 1170.1, peaks 2329.5 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1106,1106,  0,  40000,     6 }, // 1019: f15GM86; f26GM86; oGM86; Lead 7 fifths

    // Amplitude begins at    2.2, peaks  886.1 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1107,1107,  0,  40000,     0 }, // 1020: f26GM88; oGM88; oGM89; Pad 1 new age; Pad 2 warm

    // Amplitude begins at    2.6, peaks 1053.7 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1108,1108,  0,  40000,     0 }, // 1021: oGM90; oGM91; Pad 3 polysynth; Pad 4 choir

    // Amplitude begins at    6.0, peaks 2470.4 at 6.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1109,1109,  0,  40000,     6 }, // 1022: f26GM92; f26GM93; oGM92; oGM93; Pad 5 bowedpad; Pad 6 metallic

    // Amplitude begins at  336.9, peaks 3387.9 at 0.1s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 1110,1110,  0,    166,   166 }, // 1023: oGM94; Pad 7 halo

    // Amplitude begins at 2371.9, peaks 2431.7 at 0.0s,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.0s.
    { 1111,1111,  0,    926,     6 }, // 1024: f15GM97; f26GM97; oGM97; oGM98; FX 2 soundtrack; FX 3 crystal

    // Amplitude begins at    0.5, peaks 2356.5 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1112,1112,  0,  40000,     6 }, // 1025: f15GM107; f26GM107; oGM107; Koto

    // Amplitude begins at 1533.4, peaks 1569.9 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 1113,1113,  0,    253,   253 }, // 1026: oGM112; Tinkle Bell

    // Amplitude begins at 1334.5,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 351,351,  0,     66,    66 }, // 1027: oGM113; Agogo Bells

    // Amplitude begins at  424.6,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1114,1114,  0,     46,    46 }, // 1028: oGM114; Steel Drums

    // Amplitude begins at  107.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1115,1115,  0,     33,    33 }, // 1029: oGM115; Woodblock

    // Amplitude begins at 2758.0, peaks 2814.3 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 1116,1116,  0,    313,   313 }, // 1030: oGM116; oGM119; Reverse Cymbal; Taiko Drum

    // Amplitude begins at 2664.9, peaks 3050.5 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1117,1117,  0,     40,    40 }, // 1031: oGM117; oGM120; oGP37; oGP39; oGP41; oGP43; oGP45; oGP47; oGP48; oGP50; Guitar FretNoise; Hand Clap; High Floor Tom; High Tom; High-Mid Tom; Low Floor Tom; Low Tom; Low-Mid Tom; Melodic Tom; Side Stick

    // Amplitude begins at  483.6,
    // fades to 20% at 2.4s, keyoff fades to 20% in 2.4s.
    { 1118,1118,  0,   2373,  2373 }, // 1032: oGM121; Breath Noise

    // Amplitude begins at    3.1, peaks 1168.0 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 1119,1119,  0,    180,   180 }, // 1033: oGM122; Seashore

    // Amplitude begins at   62.2, peaks 1394.8 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 1120,1120,  0,    346,   346 }, // 1034: oGM124; Telephone

    // Amplitude begins at    0.8, peaks 2609.7 at 0.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.6s.
    { 1121,1121,  0,  40000,  1560 }, // 1035: oGM125; Helicopter

    // Amplitude begins at  885.6, peaks 2615.3 at 0.7s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1122,1122,  0,  40000,   146 }, // 1036: oGM126; Applause/Noise

    // Amplitude begins at 1184.0, peaks 1368.3 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 351,351, 17,     53,    53 }, // 1037: oGP35; oGP36; Ac Bass Drum; Bass Drum 1

    // Amplitude begins at  433.8,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1114,1114, 14,     60,    60 }, // 1038: oGP38; oGP40; Acoustic Snare; Electric Snare

    // Amplitude begins at  153.3,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1115,1115, 12,     66,    66 }, // 1039: oGP42; Closed High Hat

    // Amplitude begins at  133.6, peaks  391.6 at 0.0s,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 1123,1123, 14,    393,   393 }, // 1040: f15GP44; f26GP44; oGP44; Pedal High Hat

    // Amplitude begins at  133.6, peaks  399.8 at 0.0s,
    // fades to 20% at 1.5s, keyoff fades to 20% in 1.5s.
    { 1124,1124, 14,   1473,  1473 }, // 1041: f15GP46; f26GP46; oGP46; Open High Hat

    // Amplitude begins at 2203.5, peaks 2773.5 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 1116,1116,  6,    346,   346 }, // 1042: oGP51; Ride Cymbal 1

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1125,1125,158,      0,     0 }, // 1043: f15GP54; f26GP54; oGP54; Tambourine

    // Amplitude begins at 1401.1,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1126,1126,  1,      6,     6 }, // 1044: f15GP60; f26GP60; oGP60; High Bongo

    // Amplitude begins at 3020.7, peaks 3310.6 at 0.0s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 1127,1127,  1,    626,   626 }, // 1045: oGP62; oGP63; oGP64; Low Conga; Mute High Conga; Open High Conga

    // Amplitude begins at  356.8,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1128,1128, 14,     60,    60 }, // 1046: oGP69; oGP70; Cabasa; Maracas

    // Amplitude begins at 4182.5, peaks 4592.0 at 0.0s,
    // fades to 20% at 3.0s, keyoff fades to 20% in 3.0s.
    { 1129,1130,  0,   2973,  2973 }, // 1047: f12GM0; f16GM0; f54GM0; AcouGrandPiano

    // Amplitude begins at 3448.1, peaks 4110.6 at 0.0s,
    // fades to 20% at 1.5s, keyoff fades to 20% in 1.5s.
    { 1131,1131,  0,   1540,  1540 }, // 1048: f12GM1; f16GM1; f54GM1; BrightAcouGrand

    // Amplitude begins at 2452.8, peaks 3110.0 at 0.1s,
    // fades to 20% at 2.4s, keyoff fades to 20% in 2.4s.
    { 1132,1132,  0,   2373,  2373 }, // 1049: f12GM2; f16GM2; f54GM2; ElecGrandPiano

    // Amplitude begins at  828.2, peaks 2360.0 at 0.0s,
    // fades to 20% at 2.1s, keyoff fades to 20% in 2.1s.
    { 1133,1133,  0,   2133,  2133 }, // 1050: f12GM3; f16GM3; f54GM3; Honky-tonkPiano

    // Amplitude begins at 1446.9,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 1134,1134,  0,    626,   626 }, // 1051: f12GM4; f16GM4; f54GM4; Rhodes Piano

    // Amplitude begins at  855.8,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 1135,1135,  0,    353,   353 }, // 1052: f12GM5; f16GM5; f54GM5; Chorused Piano

    // Amplitude begins at  762.7, peaks  769.3 at 0.0s,
    // fades to 20% at 2.3s, keyoff fades to 20% in 2.3s.
    { 1136,1136,  0,   2333,  2333 }, // 1053: f12GM6; f16GM6; f54GM6; Harpsichord

    // Amplitude begins at  630.5,
    // fades to 20% at 0.5s, keyoff fades to 20% in 0.5s.
    { 1137,1137,  0,    506,   506 }, // 1054: f12GM8; f16GM8; f54GM8; Celesta

    // Amplitude begins at 1935.0, peaks 2170.5 at 0.0s,
    // fades to 20% at 4.2s, keyoff fades to 20% in 4.2s.
    { 1138,1138,  0,   4153,  4153 }, // 1055: f12GM9; f16GM9; f54GM9; Glockenspiel

    // Amplitude begins at  735.0, peaks 1725.3 at 0.1s,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.9s.
    { 1139,1139,  0,    853,   853 }, // 1056: f12GM10; f16GM10; f54GM10; Music box

    // Amplitude begins at 1948.2, peaks 2335.3 at 0.0s,
    // fades to 20% at 1.0s, keyoff fades to 20% in 0.0s.
    { 1140,1140,  0,    986,    13 }, // 1057: f12GM11; f16GM11; f54GM11; Vibraphone

    // Amplitude begins at 2760.8, peaks 3114.1 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 1141,1141,  0,    346,   346 }, // 1058: f12GM12; f16GM12; f54GM12; Marimba

    // Amplitude begins at  894.4,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1142,1142,  0,     33,    33 }, // 1059: f12GM13; f16GM13; f54GM13; Xylophone

    // Amplitude begins at 3212.7, peaks 3424.1 at 0.0s,
    // fades to 20% at 2.2s, keyoff fades to 20% in 2.2s.
    { 1143,1143,  0,   2166,  2166 }, // 1060: f12GM14; f16GM14; f53GM122; f54GM14; Seashore; Tubular Bells

    // Amplitude begins at 1916.3, peaks 2028.1 at 0.0s,
    // fades to 20% at 0.7s, keyoff fades to 20% in 0.7s.
    { 1144,1144,  0,    666,   666 }, // 1061: f12GM15; f16GM15; f54GM15; Dulcimer

    // Amplitude begins at 2771.2, peaks 2959.4 at 33.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1145,1145,  0,  40000,     6 }, // 1062: f12GM16; f16GM16; f54GM16; Hammond Organ

    // Amplitude begins at 1290.5, peaks 1305.3 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1146,1146,  0,  40000,     6 }, // 1063: f12GM17; f16GM17; f54GM17; Percussive Organ

    // Amplitude begins at 1816.9, peaks 2494.0 at 28.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 1147,1147,  0,  40000,   153 }, // 1064: f12GM18; f16GM18; f54GM18; Rock Organ

    // Amplitude begins at 2169.1, peaks 2236.4 at 35.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1148,1148,  0,  40000,     6 }, // 1065: f12GM19; f16GM19; f54GM19; Church Organ

    // Amplitude begins at  767.8, peaks 1041.4 at 37.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1149,1149,  0,  40000,     0 }, // 1066: f12GM20; f16GM20; f54GM20; Reed Organ

    // Amplitude begins at    0.2, peaks 1926.3 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1150,1150,  0,  40000,    26 }, // 1067: f12GM21; f16GM21; f54GM21; Accordion

    // Amplitude begins at    7.5, peaks 3521.3 at 10.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1151,1151,  0,  40000,    33 }, // 1068: f12GM22; f16GM22; f54GM22; Harmonica

    // Amplitude begins at    0.3, peaks 2028.1 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1152,1152,  0,  40000,    26 }, // 1069: f12GM23; f16GM23; f54GM23; Tango Accordion

    // Amplitude begins at 1125.5, peaks 1173.5 at 0.0s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 1153,1153,  0,    606,   606 }, // 1070: f12GM24; f16GM24; f54GM24; Acoustic Guitar1

    // Amplitude begins at 1565.7,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 1154,1154,  0,    326,   326 }, // 1071: f12GM25; f16GM25; f54GM25; Acoustic Guitar2

    // Amplitude begins at 3194.4, peaks 4214.9 at 0.0s,
    // fades to 20% at 0.8s, keyoff fades to 20% in 0.8s.
    { 1155,1155,  0,    820,   820 }, // 1072: f12GM26; f16GM26; f54GM26; Electric Guitar1

    // Amplitude begins at 2292.7, peaks 2790.9 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1156,1156,  0,    120,   120 }, // 1073: f12GM28; f16GM28; f54GM28; Electric Guitar3

    // Amplitude begins at    0.0, peaks  821.5 at 0.1s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 301,301,  0,    146,   146 }, // 1074: f12GM29; f12GM30; f12GM31; f12GM44; f12GM50; f12GM51; f12GM52; f12GM53; f12GM54; f12GM55; f12GM65; f12GM66; f12GM67; f12GM75; f16GM29; f16GM30; f16GM31; f16GM44; f16GM50; f16GM51; f16GM52; f16GM53; f16GM54; f16GM55; f16GM65; f16GM66; f16GM67; f16GM75; f54GM29; f54GM30; f54GM31; f54GM44; f54GM50; f54GM51; f54GM52; f54GM53; f54GM54; f54GM55; f54GM65; f54GM66; f54GM67; Alto Sax; Baritone Sax; Choir Aahs; Distorton Guitar; Guitar Harmonics; Orchestra Hit; Overdrive Guitar; Pan Flute; Synth Strings 1; Synth Voice; SynthStrings 2; Tenor Sax; Tremulo Strings; Voice Oohs

    // Amplitude begins at 2397.3, peaks 2948.5 at 0.8s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.4s.
    { 1157,1157,  0,  40000,   440 }, // 1075: f12GM32; f16GM32; f54GM32; Acoustic Bass

    // Amplitude begins at  608.8, peaks 1246.6 at 0.1s,
    // fades to 20% at 1.9s, keyoff fades to 20% in 1.9s.
    { 1158,1158,  0,   1913,  1913 }, // 1076: f12GM33; f16GM33; f54GM33; Electric Bass 1

    // Amplitude begins at 3660.8, peaks 4630.9 at 0.0s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 1159,1159,  0,    626,   626 }, // 1077: f12GM34; f16GM34; f54GM34; Electric Bass 2

    // Amplitude begins at 1320.6, peaks 1762.3 at 0.1s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.0s.
    { 1160,1160,  0,    160,    13 }, // 1078: f12GM35; f16GM35; f54GM35; Fretless Bass

    // Amplitude begins at 1554.3,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 1161,1161,  0,    233,   233 }, // 1079: f12GM36; f12GM37; f16GM36; f16GM37; f54GM36; Slap Bass 1; Slap Bass 2

    // Amplitude begins at  263.7, peaks 3019.5 at 0.0s,
    // fades to 20% at 1.0s, keyoff fades to 20% in 1.0s.
    { 1162,1162,  0,    960,   960 }, // 1080: f12GM38; f16GM38; f54GM38; Synth Bass 1

    // Amplitude begins at  401.5, peaks  664.6 at 0.0s,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.0s.
    { 1163,1163,  0,    380,     0 }, // 1081: f12GM39; f16GM39; f54GM39; Synth Bass 2

    // Amplitude begins at    0.0, peaks  875.6 at 0.1s,
    // fades to 20% at 0.8s, keyoff fades to 20% in 0.8s.
    { 1164,1164,  0,    760,   760 }, // 1082: b41M53; f12GM40; f16GM40; f32GM53; f41GM53; f54GM40; Violin; Voice Oohs; violin1.

    // Amplitude begins at    0.0, peaks 1231.6 at 0.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.6s.
    { 1165,1165,  0,  40000,   566 }, // 1083: f12GM41; f47GM41; Viola

    // Amplitude begins at  600.3, peaks 2156.6 at 0.0s,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.0s.
    { 1166,1167,  0,    400,     6 }, // 1084: f12GM42; f16GM42; f54GM42; Cello

    // Amplitude begins at 2811.3, peaks 3936.5 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 1168,1168,  0,    206,   206 }, // 1085: f12GM45; f16GM45; f54GM45; Pizzicato String

    // Amplitude begins at 1945.2,
    // fades to 20% at 0.5s, keyoff fades to 20% in 0.5s.
    { 1169,1170,  0,    520,   520 }, // 1086: f12GM46; Orchestral Harp

    // Amplitude begins at 1509.5, peaks 1854.4 at 0.0s,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    { 1171,1171,  0,   1186,  1186 }, // 1087: f12GM116; f12GM47; f16GM116; f16GM47; f37GM47; f54GM116; f54GM47; Taiko Drum; Timpany

    // Amplitude begins at  968.9, peaks 2806.1 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1172,1172,  0,  40000,   106 }, // 1088: f12GM48; String Ensemble1

    // Amplitude begins at    0.0, peaks 2765.9 at 5.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 2.3s.
    { 1173,1173,  0,  40000,  2260 }, // 1089: f12GM49; f16GM49; f37GM49; f54GM49; String Ensemble2

    // Amplitude begins at  652.1, peaks  907.9 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1174,1174,  0,  40000,     0 }, // 1090: f12GM56; Trumpet

    // Amplitude begins at   69.2, peaks 1076.4 at 0.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1175,1176,  0,  40000,    66 }, // 1091: f12GM57; f16GM57; f54GM57; Trombone

    // Amplitude begins at  877.6, peaks 2003.9 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1177,1178,  0,  40000,    46 }, // 1092: f12GM58; Tuba

    // Amplitude begins at   28.4, peaks 1661.7 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1179,1179,  0,  40000,     6 }, // 1093: f12GM59; f16GM59; f37GM59; f54GM59; Muted Trumpet

    // Amplitude begins at    3.1, peaks 1721.7 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1180,1181,  0,  40000,     6 }, // 1094: f12GM60; French Horn

    // Amplitude begins at  118.3, peaks 3338.0 at 0.1s,
    // fades to 20% at 3.3s, keyoff fades to 20% in 3.3s.
    { 1182,1182,  0,   3340,  3340 }, // 1095: f12GM64; f16GM64; f54GM64; Soprano Sax

    // Amplitude begins at    1.7, peaks  860.6 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1183,1183,  0,  40000,    13 }, // 1096: f12GM68; f16GM68; f47GM69; f54GM68; English Horn; Oboe

    // Amplitude begins at   53.8, peaks 1810.3 at 0.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1184,1184,  0,  40000,     6 }, // 1097: f12GM69; f16GM69; f54GM69; English Horn

    // Amplitude begins at    2.4, peaks 1046.0 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1185,1185,  0,  40000,     0 }, // 1098: f12GM70; f16GM70; f47GM70; f54GM70; Bassoon

    // Amplitude begins at 2432.3, peaks 3378.4 at 13.8s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 1186,1187,  0,  40000,   246 }, // 1099: f12GM71; f16GM71; f54GM71; Clarinet

    // Amplitude begins at    7.4, peaks 3967.1 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1188,1188,  0,  40000,    20 }, // 1100: f12GM72; f16GM72; f54GM72; Piccolo

    // Amplitude begins at    6.3, peaks 4648.0 at 0.1s,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.9s.
    { 1189,1189,  0,    906,   906 }, // 1101: f12GM73; f16GM73; f32GM72; f32GM73; f32GM74; f32GM75; f37GM72; f37GM73; f47GM73; f54GM73; Flute; Pan Flute; Piccolo; Recorder

    // Amplitude begins at    0.0, peaks 1472.9 at 0.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 1190,1191,  0,  40000,   193 }, // 1102: f12GM74; f16GM74; f54GM74; Recorder

    // Amplitude begins at  777.6, peaks  877.5 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1192,1193,  0,  40000,    20 }, // 1103: f12GM77; f16GM77; f53GM77; f54GM77; Shakuhachi

    // Amplitude begins at 1110.1, peaks 3351.2 at 0.1s,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.9s.
    { 1194,1195,  0,    940,   940 }, // 1104: f12GM84; f16GM84; f54GM84; Lead 5 charang

    // Amplitude begins at 7498.1, peaks 9450.9 at 0.0s,
    // fades to 20% at 1.8s, keyoff fades to 20% in 1.8s.
    { 1196,1197,  0,   1800,  1800 }, // 1105: f12GM106; f16GM106; f54GM106; Shamisen

    // Amplitude begins at 4507.3, peaks 7555.8 at 0.0s,
    // fades to 20% at 0.8s, keyoff fades to 20% in 0.8s.
    { 1198,1199,  0,    766,   766 }, // 1106: f12GM107; f16GM107; f53GM106; f54GM107; Koto; Shamisen

    // Amplitude begins at  844.6, peaks 1122.1 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1200,1201,  0,  40000,    13 }, // 1107: f12GM111; f16GM111; f53GM82; f54GM111; Lead 3 calliope; Shanai

    // Amplitude begins at  802.9,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 1202,1202,  0,    153,   153 }, // 1108: f12GM115; f16GM115; f54GM115; Woodblock

    // Amplitude begins at 1197.6,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1203,1203,  0,     40,    40 }, // 1109: f12GP31; f12GP36; f16GP31; f16GP36; f37GP31; f37GP36; f54GP31; f54GP36; f54GP60; f54GP61; f54GP62; f54GP63; f54GP64; f54GP65; f54GP66; Bass Drum 1; High Bongo; High Timbale; Low Bongo; Low Conga; Low Timbale; Mute High Conga; Open High Conga

    // Amplitude begins at  364.3,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1204,1204,  6,     46,    46 }, // 1110: f12GP33; f16GP33; f54GP33

    // Amplitude begins at 1681.7, peaks 1901.4 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1205,1206,  0,     20,    20 }, // 1111: f12GP35; f54GP35; Ac Bass Drum

    // Amplitude begins at  359.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1207,1207,  5,      6,     6 }, // 1112: f12GP37; f16GP37; f54GP37; Side Stick

    // Amplitude begins at  776.4,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1208,1208,  0,     80,    80 }, // 1113: f12GP38; f16GP38; f54GP38; Acoustic Snare

    // Amplitude begins at  528.5,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1209,1209,  2,      6,     6 }, // 1114: f12GP39; f16GP39; f54GP39; Hand Clap

    // Amplitude begins at  499.1, peaks  748.2 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1210,1210,  2,    100,   100 }, // 1115: f12GP40; f16GP40; f37GP40; f54GP40; Electric Snare

    // Amplitude begins at 1149.7,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1211,1211,  1,     20,    20 }, // 1116: f12GP41; f12GP43; f12GP45; f12GP47; f12GP48; f12GP50; f16GP41; f16GP43; f16GP45; f16GP47; f16GP48; f16GP50; f37GP41; f37GP43; f37GP45; f37GP47; f54GP41; f54GP43; f54GP45; f54GP47; f54GP48; f54GP50; High Floor Tom; High Tom; High-Mid Tom; Low Floor Tom; Low Tom; Low-Mid Tom

    // Amplitude begins at  586.6,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1212,1213,  2,     46,    46 }, // 1117: f12GP42; f54GP42; Closed High Hat

    // Amplitude begins at  806.8,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1214,1214,  5,     26,    26 }, // 1118: f12GP44; f12GP46; f12GP54; f16GP42; f16GP44; f16GP46; f16GP54; f54GP44; f54GP46; f54GP54; Closed High Hat; Open High Hat; Pedal High Hat; Tambourine

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1215,1215,135,      0,     0 }, // 1119: f12GP49; f16GP49; f54GP49; Crash Cymbal 1

    // Amplitude begins at    0.0, peaks  817.9 at 0.2s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 301,301,  7,    186,   186 }, // 1120: f12GP51; f16GP51; f54GP51; Ride Cymbal 1

    // Amplitude begins at  978.3,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1216,1216,  0,     40,    40 }, // 1121: f12GP56; f16GP56; f54GP56; Cow Bell

    // Amplitude begins at  795.1,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1217,1217,  0,     20,    20 }, // 1122: f12GP60; High Bongo

    // Amplitude begins at 1037.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1218,1218,  0,     20,    20 }, // 1123: f12GP61; Low Bongo

    // Amplitude begins at 1496.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1219,1219,  1,     13,    13 }, // 1124: f12GP60; f12GP61; f16GP60; f16GP61; High Bongo; Low Bongo

    // Amplitude begins at  497.8,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1220,1220,  0,     26,    26 }, // 1125: f12GP63; f16GP63; f16GP64; Low Conga; Open High Conga

    // Amplitude begins at  580.9,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1221,1221,  1,      6,     6 }, // 1126: f12GP64; Low Conga

    // Amplitude begins at 2602.6, peaks 2842.4 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1222,1222,  0,  40000,   106 }, // 1127: f13GM0; f50GM0; AcouGrandPiano

    // Amplitude begins at 3880.7, peaks 4636.6 at 0.0s,
    // fades to 20% at 0.7s, keyoff fades to 20% in 0.0s.
    { 1223,1223,  0,    660,    13 }, // 1128: f13GM1; f50GM1; BrightAcouGrand

    // Amplitude begins at 3812.4,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.4s.
    { 1224,1224,  0,  40000,   366 }, // 1129: f13GM2; f50GM2; ElecGrandPiano

    // Amplitude begins at 1235.0,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 1225,1225,  0,    380,   380 }, // 1130: f13GM3; f50GM3; Honky-tonkPiano

    // Amplitude begins at 1441.4, peaks 1470.0 at 0.1s,
    // fades to 20% at 1.7s, keyoff fades to 20% in 1.7s.
    { 1226,1226,  0,   1700,  1700 }, // 1131: f13GM4; f50GM4; Rhodes Piano

    // Amplitude begins at 2814.7, peaks 2874.3 at 0.0s,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    { 1227,1227,  0,   1220,  1220 }, // 1132: f13GM5; f50GM5; Chorused Piano

    // Amplitude begins at  604.8, peaks  636.7 at 0.0s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 1228,1228,  0,    606,   606 }, // 1133: f13GM6; f50GM6; Harpsichord

    // Amplitude begins at   18.3, peaks  467.7 at 0.0s,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    { 1229,1229,  0,   1226,  1226 }, // 1134: f13GM7; f50GM7; Clavinet

    // Amplitude begins at  711.7, peaks  846.4 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 1230,1230,  0,    180,   180 }, // 1135: f13GM8; f50GM8; Celesta

    // Amplitude begins at 2836.2, peaks 2844.7 at 0.0s,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 1231,1231,  0,    440,   440 }, // 1136: f13GM9; f50GM9; Glockenspiel

    // Amplitude begins at 2504.8, peaks 2897.2 at 0.0s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 1232,1232,  0,    573,   573 }, // 1137: f13GM10; f50GM10; Music box

    // Amplitude begins at 1174.3, peaks 1206.1 at 0.5s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 1233,1233,  0,  40000,   246 }, // 1138: f13GM11; f50GM11; Vibraphone

    // Amplitude begins at 1728.6, peaks 3110.1 at 0.1s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 1234,1234,  0,    206,   206 }, // 1139: f13GM12; f50GM12; Marimba

    // Amplitude begins at  985.6,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1235,1235,  0,    133,   133 }, // 1140: f13GM13; f50GM13; Xylophone

    // Amplitude begins at  830.4, peaks  856.4 at 0.0s,
    // fades to 20% at 0.8s, keyoff fades to 20% in 0.8s.
    { 1236,1236,  0,    820,   820 }, // 1141: f13GM14; f50GM14; Tubular Bells

    // Amplitude begins at  107.2, peaks 1304.2 at 0.1s,
    // fades to 20% at 0.8s, keyoff fades to 20% in 0.8s.
    { 1237,1237,  0,    793,   793 }, // 1142: f13GM15; f50GM15; Dulcimer

    // Amplitude begins at  831.9, peaks  990.8 at 30.5s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1238,1238,  0,  40000,     0 }, // 1143: f13GM16; f50GM16; Hammond Organ

    // Amplitude begins at  441.3, peaks  466.7 at 1.5s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1239,1239,  0,  40000,     0 }, // 1144: f13GM17; f50GM17; Percussive Organ

    // Amplitude begins at    0.3, peaks  790.1 at 0.1s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1240,1240,  0,     93,    93 }, // 1145: f13GM18; f50GM18; Rock Organ

    // Amplitude begins at    0.5, peaks 1311.9 at 27.5s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1241,1241,  0,  40000,     6 }, // 1146: f13GM19; f50GM19; Church Organ

    // Amplitude begins at 1272.4, peaks 1370.1 at 30.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1242,1242,  0,  40000,     6 }, // 1147: f13GM20; f50GM20; Reed Organ

    // Amplitude begins at    0.3, peaks  716.6 at 21.7s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1243,1243,  0,  40000,     6 }, // 1148: f13GM21; f50GM21; Accordion

    // Amplitude begins at    0.0, peaks 1394.4 at 9.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1244,1244,  0,  40000,     6 }, // 1149: f13GM22; f50GM22; Harmonica

    // Amplitude begins at 2181.7, peaks 3400.4 at 31.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1245,1245,  0,  40000,     6 }, // 1150: f13GM23; f50GM23; Tango Accordion

    // Amplitude begins at 1751.4, peaks 1778.6 at 0.0s,
    // fades to 20% at 0.5s, keyoff fades to 20% in 0.5s.
    { 1246,1246,  0,    466,   466 }, // 1151: f13GM24; f50GM24; Acoustic Guitar1

    // Amplitude begins at 1729.7,
    // fades to 20% at 0.7s, keyoff fades to 20% in 0.7s.
    { 1247,1247,  0,    680,   680 }, // 1152: f13GM25; f50GM25; Acoustic Guitar2

    // Amplitude begins at 1819.6,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 1248,1248,  0,    346,   346 }, // 1153: f13GM26; f50GM26; Electric Guitar1

    // Amplitude begins at  797.8, peaks  890.1 at 0.0s,
    // fades to 20% at 1.9s, keyoff fades to 20% in 1.9s.
    { 1249,1249,  0,   1886,  1886 }, // 1154: f13GM27; f50GM27; Electric Guitar2

    // Amplitude begins at  341.4, peaks  388.9 at 0.0s,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 1250,1250,  0,    406,   406 }, // 1155: f13GM28; f50GM28; Electric Guitar3

    // Amplitude begins at 1068.9, peaks 1192.2 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1251,1251,  0,  40000,     0 }, // 1156: f13GM29; f50GM29; Overdrive Guitar

    // Amplitude begins at 1068.9, peaks 1084.7 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1252,1252,  0,  40000,     0 }, // 1157: f13GM30; f50GM30; Distorton Guitar

    // Amplitude begins at    2.8, peaks 1030.9 at 0.1s,
    // fades to 20% at 1.1s, keyoff fades to 20% in 1.1s.
    { 1253,1253,  0,   1100,  1100 }, // 1158: f13GM31; f50GM31; Guitar Harmonics

    // Amplitude begins at 1731.8, peaks 2093.0 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1254,1254,  0,  40000,     0 }, // 1159: f13GM32; f50GM32; Acoustic Bass

    // Amplitude begins at  747.1, peaks 1551.0 at 0.0s,
    // fades to 20% at 2.1s, keyoff fades to 20% in 2.1s.
    { 1255,1255,  0,   2113,  2113 }, // 1160: f13GM33; f50GM33; Electric Bass 1

    // Amplitude begins at  901.5, peaks 1124.1 at 0.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1256,1256,  0,  40000,    53 }, // 1161: f13GM34; f50GM34; Electric Bass 2

    // Amplitude begins at 3272.4, peaks 3582.0 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1257,1257,  0,  40000,     6 }, // 1162: f13GM35; f50GM35; Fretless Bass

    // Amplitude begins at  825.8, peaks  968.9 at 0.1s,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.9s.
    { 1258,1258,  0,    913,   913 }, // 1163: f13GM36; f50GM36; Slap Bass 1

    // Amplitude begins at 1468.4, peaks 1898.1 at 0.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1259,1259,  0,  40000,    53 }, // 1164: f13GM37; f50GM37; Slap Bass 2

    // Amplitude begins at  840.2, peaks  924.5 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1260,1260,  0,  40000,     0 }, // 1165: f13GM38; f50GM38; Synth Bass 1

    // Amplitude begins at 1986.3, peaks 2145.3 at 0.1s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.0s.
    { 1261,1261,  0,    140,    26 }, // 1166: f13GM39; f50GM39; Synth Bass 2

    // Amplitude begins at  177.2, peaks 1827.0 at 0.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1262,1262,  0,  40000,    40 }, // 1167: f13GM40; Violin

    // Amplitude begins at 1579.8, peaks 3431.0 at 19.7s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1263,1263,  0,  40000,    73 }, // 1168: f13GM41; f50GM41; Viola

    // Amplitude begins at    0.0, peaks 1269.7 at 0.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1264,1264,  0,  40000,     0 }, // 1169: f13GM42; f50GM42; Cello

    // Amplitude begins at    0.4, peaks 1539.6 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1265,1265,  0,  40000,     0 }, // 1170: f13GM43; f50GM43; Contrabass

    // Amplitude begins at 3083.4, peaks 3194.1 at 0.0s,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.0s.
    { 1266,1266,  0,    853,     6 }, // 1171: f13GM44; f50GM44; Tremulo Strings

    // Amplitude begins at 1311.1,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 1267,1267,  0,    213,   213 }, // 1172: f13GM45; f50GM45; Pizzicato String

    // Amplitude begins at 1174.1, peaks 1693.1 at 0.0s,
    // fades to 20% at 0.7s, keyoff fades to 20% in 0.7s.
    { 1268,1268,  0,    733,   733 }, // 1173: f13GM46; f50GM46; Orchestral Harp

    // Amplitude begins at 1762.4,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 1269,1269,  0,    166,   166 }, // 1174: f13GM47; f50GM47; Timpany

    // Amplitude begins at 3083.4, peaks 3193.2 at 0.0s,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.0s.
    { 1270,1270,  0,    853,    13 }, // 1175: f13GM48; f50GM48; String Ensemble1

    // Amplitude begins at    0.0, peaks  439.9 at 0.7s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 1271,1271,  0,  40000,   240 }, // 1176: f13GM49; f50GM49; String Ensemble2

    // Amplitude begins at 1516.3, peaks 2121.7 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1272,1272,  0,  40000,    20 }, // 1177: f13GM50; f50GM50; Synth Strings 1

    // Amplitude begins at    0.0, peaks  925.0 at 0.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1273,1273,  0,  40000,    13 }, // 1178: f13GM51; f50GM51; SynthStrings 2

    // Amplitude begins at  994.3, peaks 3768.9 at 4.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1274,1274,  0,  40000,   133 }, // 1179: f13GM52; f50GM52; Choir Aahs

    // Amplitude begins at    6.8, peaks 2509.8 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1275,1275,  0,  40000,    26 }, // 1180: f13GM53; f50GM53; Voice Oohs

    // Amplitude begins at    0.0, peaks 3986.6 at 36.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1276,1276,  0,  40000,    93 }, // 1181: f13GM54; f50GM54; Synth Voice

    // Amplitude begins at 1517.5, peaks 1790.7 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 1277,1277,  0,    160,   160 }, // 1182: f13GM55; f50GM55; Orchestra Hit

    // Amplitude begins at 1034.3, peaks 1467.1 at 17.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1278,1278,  0,  40000,     0 }, // 1183: f13GM56; f50GM56; Trumpet

    // Amplitude begins at  735.1, peaks  943.2 at 0.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1279,1279,  0,  40000,     0 }, // 1184: f13GM57; f50GM57; Trombone

    // Amplitude begins at  258.9, peaks 2277.6 at 0.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1280,1280,  0,  40000,    20 }, // 1185: f13GM58; f50GM58; Tuba

    // Amplitude begins at  256.3, peaks  989.9 at 0.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1281,1281,  0,  40000,    26 }, // 1186: f13GM59; f50GM59; Muted Trumpet

    // Amplitude begins at 1036.7, peaks 1366.7 at 1.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1282,1282,  0,  40000,    66 }, // 1187: f13GM60; f50GM60; French Horn

    // Amplitude begins at  733.0, peaks 3543.0 at 0.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1283,1283,  0,  40000,    20 }, // 1188: f13GM61; f50GM61; Brass Section

    // Amplitude begins at  732.3, peaks  921.6 at 0.0s,
    // fades to 20% at 1.8s, keyoff fades to 20% in 1.8s.
    { 1284,1284,  0,   1800,  1800 }, // 1189: f13GM62; f50GM62; Synth Brass 1

    // Amplitude begins at  733.0, peaks  944.3 at 27.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1285,1285,  0,  40000,     6 }, // 1190: f13GM63; f50GM63; Synth Brass 2

    // Amplitude begins at    0.3, peaks 1100.1 at 23.7s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1286,1286,  0,  40000,     6 }, // 1191: f13GM64; f50GM64; Soprano Sax

    // Amplitude begins at    0.3, peaks  778.0 at 23.7s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1287,1287,  0,  40000,     6 }, // 1192: f13GM65; f50GM65; Alto Sax

    // Amplitude begins at    0.0, peaks  778.0 at 23.7s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1288,1288,  0,  40000,    20 }, // 1193: f13GM66; f50GM66; Tenor Sax

    // Amplitude begins at    0.0, peaks  778.0 at 23.7s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1289,1289,  0,  40000,    20 }, // 1194: f13GM67; f50GM67; Baritone Sax

    // Amplitude begins at 1321.7, peaks 4321.1 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.4s.
    { 1290,1290,  0,  40000,   353 }, // 1195: f13GM68; f50GM68; Oboe

    // Amplitude begins at    7.9, peaks 2789.2 at 13.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1291,1291,  0,  40000,    66 }, // 1196: f13GM69; f50GM69; English Horn

    // Amplitude begins at    0.5, peaks 3191.9 at 30.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1292,1292,  0,  40000,     6 }, // 1197: f13GM70; f50GM70; Bassoon

    // Amplitude begins at    0.0, peaks 1063.6 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1293,1293,  0,  40000,    53 }, // 1198: f13GM71; f50GM71; Clarinet

    // Amplitude begins at    0.0, peaks 4408.2 at 19.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 1294,1294,  0,  40000,   180 }, // 1199: f13GM72; f50GM72; Piccolo

    // Amplitude begins at    0.8, peaks 4186.9 at 0.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1295,1295,  0,  40000,     6 }, // 1200: f13GM73; f50GM73; Flute

    // Amplitude begins at    0.0, peaks 3770.5 at 29.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1296,1296,  0,  40000,    20 }, // 1201: f13GM74; f50GM74; Recorder

    // Amplitude begins at    0.0, peaks  890.2 at 0.1s,
    // fades to 20% at 1.0s, keyoff fades to 20% in 1.0s.
    { 1297,1297,  0,   1046,  1046 }, // 1202: f13GM75; f50GM75; Pan Flute

    // Amplitude begins at  117.1, peaks 1791.6 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1298,1298,  0,  40000,     6 }, // 1203: f13GM76; f50GM76; Bottle Blow

    // Amplitude begins at    0.0, peaks  929.2 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1299,1299,  0,  40000,     0 }, // 1204: f13GM77; f50GM77; Shakuhachi

    // Amplitude begins at    0.0, peaks  713.5 at 0.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1300,1300,  0,  40000,     6 }, // 1205: f13GM78; Whistle

    // Amplitude begins at    7.2, peaks 5589.8 at 23.8s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1301,1301,  0,  40000,    13 }, // 1206: f13GM79; f50GM79; Ocarina

    // Amplitude begins at 1454.7, peaks 1832.8 at 19.5s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1302,1302,  0,  40000,     6 }, // 1207: f13GM80; f50GM80; Lead 1 squareea

    // Amplitude begins at  558.6, peaks 3297.5 at 0.1s,
    // fades to 20% at 1.0s, keyoff fades to 20% in 1.0s.
    { 1303,1303,  0,    953,   953 }, // 1208: f13GM81; f50GM81; Lead 2 sawtooth

    // Amplitude begins at    6.6, peaks 1731.3 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1304,1304,  0,  40000,     6 }, // 1209: f13GM82; f50GM82; Lead 3 calliope

    // Amplitude begins at    0.0, peaks  414.4 at 0.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1305,1305,  0,  40000,     6 }, // 1210: f13GM83; f50GM83; Lead 4 chiff

    // Amplitude begins at 1349.4,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1306,1306,  0,  40000,    13 }, // 1211: f13GM84; f50GM84; Lead 5 charang

    // Amplitude begins at    0.0, peaks 1696.7 at 21.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1307,1307,  0,  40000,    13 }, // 1212: f13GM85; f50GM85; Lead 6 voice

    // Amplitude begins at    0.0, peaks  683.4 at 0.1s,
    // fades to 20% at 0.7s, keyoff fades to 20% in 0.7s.
    { 1308,1308,  0,    740,   740 }, // 1213: f13GM86; f50GM86; Lead 7 fifths

    // Amplitude begins at 3912.6,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.9s.
    { 1309,1309,  0,    906,   906 }, // 1214: f13GM87; f50GM87; Lead 8 brass

    // Amplitude begins at    0.0, peaks 2955.5 at 0.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1310,1310,  0,  40000,   106 }, // 1215: f13GM88; f50GM88; Pad 1 new age

    // Amplitude begins at    0.0, peaks 3206.5 at 0.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1311,1311,  0,  40000,    66 }, // 1216: f13GM89; f50GM89; Pad 2 warm

    // Amplitude begins at 1036.7, peaks 1432.5 at 0.2s,
    // fades to 20% at 0.7s, keyoff fades to 20% in 0.0s.
    { 1312,1312,  0,    726,    26 }, // 1217: f13GM90; f50GM90; Pad 3 polysynth

    // Amplitude begins at    0.0, peaks 4677.6 at 1.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.4s.
    { 1313,1313,  0,  40000,   366 }, // 1218: f13GM91; f50GM91; Pad 4 choir

    // Amplitude begins at  733.0, peaks 1115.3 at 26.7s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.0s.
    { 1314,1314,  0,  40000,  1000 }, // 1219: f13GM92; f50GM92; Pad 5 bowedpad

    // Amplitude begins at    0.0, peaks 2424.3 at 1.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1315,1315,  0,  40000,   113 }, // 1220: f13GM93; f50GM93; Pad 6 metallic

    // Amplitude begins at    0.0, peaks 4375.4 at 0.3s,
    // fades to 20% at 2.8s, keyoff fades to 20% in 2.8s.
    { 1316,1316,  0,   2780,  2780 }, // 1221: f13GM94; f50GM94; Pad 7 halo

    // Amplitude begins at    0.0, peaks 3705.5 at 1.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1317,1317,  0,  40000,    40 }, // 1222: f13GM95; f50GM95; Pad 8 sweep

    // Amplitude begins at 2911.1, peaks 3066.7 at 0.1s,
    // fades to 20% at 3.7s, keyoff fades to 20% in 3.7s.
    { 1318,1318,  0,   3726,  3726 }, // 1223: f13GM96; f50GM96; FX 1 rain

    // Amplitude begins at    0.0, peaks 1463.5 at 0.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1319,1319,  0,  40000,    20 }, // 1224: f13GM97; f50GM97; FX 2 soundtrack

    // Amplitude begins at  748.1, peaks 1068.1 at 0.0s,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 1320,1320,  0,    433,   433 }, // 1225: f13GM98; f50GM98; FX 3 crystal

    // Amplitude begins at 1949.1,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    { 1321,1321,  0,   1240,  1240 }, // 1226: f13GM99; f50GM99; FX 4 atmosphere

    // Amplitude begins at 2414.5, peaks 2797.4 at 0.0s,
    // fades to 20% at 1.0s, keyoff fades to 20% in 1.0s.
    { 1322,1322,  0,    986,   986 }, // 1227: f13GM100; f50GM100; FX 5 brightness

    // Amplitude begins at    0.0, peaks 1404.8 at 7.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.3s.
    { 1323,1323,  0,  40000,   326 }, // 1228: f13GM101; f50GM101; FX 6 goblins

    // Amplitude begins at 1700.2, peaks 1838.3 at 0.0s,
    // fades to 20% at 1.1s, keyoff fades to 20% in 1.1s.
    { 1324,1324,  0,   1073,  1073 }, // 1229: f13GM102; f50GM102; FX 7 echoes

    // Amplitude begins at  283.1, peaks  462.8 at 1.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.6s.
    { 1325,1325,  0,  40000,   606 }, // 1230: f13GM103; f50GM103; FX 8 sci-fi

    // Amplitude begins at 1565.5, peaks 1616.9 at 0.0s,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    { 1326,1326,  0,   1153,  1153 }, // 1231: f13GM104; f50GM104; Sitar

    // Amplitude begins at  476.5,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1327,1327,  0,     13,    13 }, // 1232: f13GM105; f50GM105; Banjo

    // Amplitude begins at  379.8,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 1328,1328,  0,    406,   406 }, // 1233: f13GM106; f50GM106; Shamisen

    // Amplitude begins at 3085.4,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 1329,1329,  0,    206,   206 }, // 1234: f13GM107; f50GM107; Koto

    // Amplitude begins at 1532.1,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1330,1330,  0,    146,   146 }, // 1235: f13GM108; f50GM108; Kalimba

    // Amplitude begins at    0.0, peaks  532.0 at 1.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1331,1331,  0,  40000,     6 }, // 1236: f13GM109; f50GM109; Bagpipe

    // Amplitude begins at    0.0, peaks  728.2 at 7.5s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1332,1332,  0,  40000,     6 }, // 1237: f13GM110; f50GM110; Fiddle

    // Amplitude begins at    0.0, peaks 1380.6 at 0.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1333,1333,  0,  40000,    66 }, // 1238: f13GM111; f50GM111; Shanai

    // Amplitude begins at 1097.0,
    // fades to 20% at 1.4s, keyoff fades to 20% in 1.4s.
    { 1334,1334,  0,   1426,  1426 }, // 1239: f13GM112; f50GM112; Tinkle Bell

    // Amplitude begins at  630.5, peaks 2612.7 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1335,1335,  0,    113,   113 }, // 1240: f13GM113; f50GM113; Agogo Bells

    // Amplitude begins at    0.8, peaks 2916.4 at 0.1s,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 1336,1336,  0,    380,   380 }, // 1241: f13GM114; f50GM114; Steel Drums

    // Amplitude begins at  711.3,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1337,1337,  0,     20,    20 }, // 1242: f13GM115; f50GM115; Woodblock

    // Amplitude begins at 1285.7,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1338,1338,  0,     33,    33 }, // 1243: f13GM116; f50GM116; Taiko Drum

    // Amplitude begins at 1713.3,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1339,1339,  0,     86,    86 }, // 1244: f13GM117; f50GM117; Melodic Tom

    // Amplitude begins at  617.5, peaks  688.1 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1340,1340,  0,     40,    40 }, // 1245: f13GM118; f50GM118; Synth Drum

    // Amplitude begins at    0.0, peaks  456.3 at 2.1s,
    // fades to 20% at 2.3s, keyoff fades to 20% in 2.3s.
    { 1341,1341,  0,   2333,  2333 }, // 1246: f13GM119; f50GM119; Reverse Cymbal

    // Amplitude begins at   90.3, peaks 1449.4 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1342,1342,  0,    100,   100 }, // 1247: f13GM120; f50GM120; Guitar FretNoise

    // Amplitude begins at    0.0, peaks  476.9 at 0.3s,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 1343,1343,  0,    366,   366 }, // 1248: f13GM121; f50GM121; Breath Noise

    // Amplitude begins at    0.0, peaks  834.7 at 1.1s,
    // fades to 20% at 1.8s, keyoff fades to 20% in 1.8s.
    { 1344,1344,  0,   1753,  1753 }, // 1249: f13GM122; f50GM122; Seashore

    // Amplitude begins at    0.0, peaks 1492.2 at 0.1s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 1345,1345,  0,    320,   320 }, // 1250: f13GM123; f50GM123; Bird Tweet

    // Amplitude begins at  937.7,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1346,1346,  0,  40000,     6 }, // 1251: f13GM124; f50GM124; Telephone

    // Amplitude begins at    0.0, peaks  903.6 at 34.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1347,1347,  0,  40000,     0 }, // 1252: f13GM125; f50GM125; Helicopter

    // Amplitude begins at    0.0, peaks  881.8 at 11.5s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.6s.
    { 1348,1348,  0,  40000,   553 }, // 1253: f13GM126; f50GM126; Applause/Noise

    // Amplitude begins at  837.0,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1349,1349,  0,    146,   146 }, // 1254: f13GM127; f50GM127; Gunshot

    // Amplitude begins at 1132.3,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1350,1350,  0,     26,    26 }, // 1255: f13GP35; f13GP36; f50GP35; f50GP36; Ac Bass Drum; Bass Drum 1

    // Amplitude begins at 1076.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1351,1351,  5,     20,    20 }, // 1256: f13GP37; f50GP37; Side Stick

    // Amplitude begins at  548.3, peaks  709.9 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1352,1352, 46,     46,    46 }, // 1257: f13GP38; f50GP38; Acoustic Snare

    // Amplitude begins at  171.5, peaks  670.5 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1353,1353,  5,     40,    40 }, // 1258: f13GP39; f50GP39; Hand Clap

    // Amplitude begins at  592.6,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1354,1354,  0,     46,    46 }, // 1259: f13GP40; f50GP40; Electric Snare

    // Amplitude begins at 1468.5,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1355,1355,  0,     40,    40 }, // 1260: f13GP41; f13GP43; f13GP45; f13GP47; f13GP48; f13GP50; f50GP41; f50GP43; f50GP45; f50GP47; f50GP48; f50GP50; High Floor Tom; High Tom; High-Mid Tom; Low Floor Tom; Low Tom; Low-Mid Tom

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1356,1356,129,      0,     0 }, // 1261: f13GP42; f50GP42; Closed High Hat

    // Amplitude begins at    7.7, peaks  548.2 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1357,1357,  1,     33,    33 }, // 1262: f13GP44; f50GP44; Pedal High Hat

    // Amplitude begins at  466.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1358,1358,  1,     40,    40 }, // 1263: f13GP46; f50GP46; Open High Hat

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1359,1359,129,      0,     0 }, // 1264: f13GP49; f13GP57; f50GP49; f50GP57; Crash Cymbal 1; Crash Cymbal 2

    // Amplitude begins at  107.3,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 1360,1360,  0,    333,   333 }, // 1265: f13GP51; f13GP53; f13GP59; f50GP51; f50GP53; f50GP59; Ride Bell; Ride Cymbal 1; Ride Cymbal 2

    // Amplitude begins at  439.8, peaks  493.8 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 1361,1361,  1,    266,   266 }, // 1266: f13GP52; f50GP52; Chinese Cymbal

    // Amplitude begins at   26.8, peaks  427.4 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1362,1362,  0,     93,    93 }, // 1267: f13GP54; f50GP54; Tambourine

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1363,1363,129,      0,     0 }, // 1268: f13GP55; f50GP55; Splash Cymbal

    // Amplitude begins at  811.5,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1364,1364,  9,     46,    46 }, // 1269: f13GP56; f50GP56; Cow Bell

    // Amplitude begins at  823.2,
    // fades to 20% at 0.7s, keyoff fades to 20% in 0.7s.
    { 1365,1365, 16,    733,   733 }, // 1270: f13GP58; f50GP58; Vibraslap

    // Amplitude begins at 1757.4,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1366,1366,  3,      6,     6 }, // 1271: f13GP60; f50GP60; High Bongo

    // Amplitude begins at 2018.9, peaks 2313.6 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1367,1367,  3,     13,    13 }, // 1272: f13GP61; f50GP61; Low Bongo

    // Amplitude begins at  277.8,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1368,1368,  3,     13,    13 }, // 1273: f13GP62; f50GP62; Mute High Conga

    // Amplitude begins at 2467.1,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1369,1369,  2,     13,    13 }, // 1274: f13GP63; f13GP64; f50GP63; f50GP64; Low Conga; Open High Conga

    // Amplitude begins at  883.5, peaks 1234.0 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1370,1370,  1,     33,    33 }, // 1275: f13GP65; f13GP66; f50GP65; f50GP66; High Timbale; Low Timbale

    // Amplitude begins at   53.9, peaks 2796.9 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1371,1371, 37,    120,   120 }, // 1276: f13GP67; f13GP68; f50GP67; f50GP68; High Agogo; Low Agogo

    // Amplitude begins at   11.5, peaks  397.9 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1372,1372, 17,     53,    53 }, // 1277: f13GP69; f50GP69; Cabasa

    // Amplitude begins at  184.5, peaks  311.2 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1373,1373,  0,     40,    40 }, // 1278: f13GP70; f50GP70; Maracas

    // Amplitude begins at  873.7, peaks 1094.6 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1374,1374, 76,     86,    86 }, // 1279: f13GP71; f50GP71; Short Whistle

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1375,1375,140,      0,     0 }, // 1280: f13GP72; f50GP72; Long Whistle

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1376,1376,139,      0,     0 }, // 1281: f13GP73; f50GP73; Short Guiro

    // Amplitude begins at    0.0, peaks  323.4 at 0.1s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 1377,1377,  0,    186,   186 }, // 1282: f13GP74; f50GP74; Long Guiro

    // Amplitude begins at 2459.0,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1378,1378, 29,     80,    80 }, // 1283: f13GP75; f50GP75; Claves

    // Amplitude begins at  747.2,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1337,1337,  7,      6,     6 }, // 1284: f13GP76; f13GP77; f50GP76; f50GP77; High Wood Block; Low Wood Block

    // Amplitude begins at    0.0, peaks 2418.9 at 0.7s,
    // fades to 20% at 0.7s, keyoff fades to 20% in 0.7s.
    { 1379,1379,  2,    713,   713 }, // 1285: f13GP78; f50GP78; Mute Cuica

    // Amplitude begins at    0.0, peaks 3803.0 at 0.7s,
    // fades to 20% at 0.8s, keyoff fades to 20% in 0.8s.
    { 1380,1380,  1,    813,   813 }, // 1286: f13GP79; f50GP79; Open Cuica

    // Amplitude begins at   46.9, peaks 1304.4 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1381,1381,  4,     80,    80 }, // 1287: f13GP80; f50GP80; Mute Triangle

    // Amplitude begins at 1130.2, peaks 1259.8 at 0.0s,
    // fades to 20% at 0.7s, keyoff fades to 20% in 0.7s.
    { 1382,1382,  6,    733,   733 }, // 1288: f13GP81; f50GP81; Open Triangle

    // Amplitude begins at 1152.1, peaks 2268.8 at 0.1s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 1383,1383,  0,    340,   340 }, // 1289: f13GP82; f50GP82; Shaker

    // Amplitude begins at  447.2, peaks  448.3 at 0.0s,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 1384,1384, 14,    386,   386 }, // 1290: f13GP83; f13GP84; f50GP83; f50GP84; Bell Tree; Jingle Bell

    // Amplitude begins at  737.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1385,1385,  3,     20,    20 }, // 1291: f13GP85; f50GP85; Castanets

    // Amplitude begins at  961.2,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1386,1386, 17,      6,     6 }, // 1292: f13GP86; f50GP86; Mute Surdu

    // Amplitude begins at  834.1,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1387,1387, 17,     13,    13 }, // 1293: f13GP87; f50GP87; Open Surdu

    // Amplitude begins at  519.3, peaks 2806.4 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1388,1388, 37,    113,   113 }, // 1294: f13GP88; f50GP88

    // Amplitude begins at  765.3,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1389,1389, 14,     53,    53 }, // 1295: f13GP89; f50GP89

    // Amplitude begins at  405.9,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1390,1390,  0,     20,    20 }, // 1296: f13GP90; f50GP90

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1391,1391,129,      0,     0 }, // 1297: f13GP91; f50GP91

    // Amplitude begins at 4205.3,
    // fades to 20% at 3.0s, keyoff fades to 20% in 3.0s.
    { 1392,1392,  0,   3006,  3006 }, // 1298: f15GM0; f26GM0; AcouGrandPiano

    // Amplitude begins at 3070.8, peaks 4460.3 at 0.0s,
    // fades to 20% at 1.3s, keyoff fades to 20% in 1.3s.
    { 1393,1393,  0,   1326,  1326 }, // 1299: f15GM1; f26GM1; BrightAcouGrand

    // Amplitude begins at 2058.4, peaks 2089.6 at 0.0s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 1394,1394,  0,    566,   566 }, // 1300: f15GM2; f26GM2; ElecGrandPiano

    // Amplitude begins at 2765.9, peaks 2792.0 at 0.0s,
    // fades to 20% at 0.8s, keyoff fades to 20% in 0.8s.
    { 1395,1395,  0,    820,   820 }, // 1301: f15GM3; f26GM3; Honky-tonkPiano

    // Amplitude begins at 3194.0, peaks 3276.0 at 0.0s,
    // fades to 20% at 0.8s, keyoff fades to 20% in 0.8s.
    { 1396,1396,  0,    800,   800 }, // 1302: f15GM4; f26GM4; Rhodes Piano

    // Amplitude begins at 2812.3, peaks 2950.7 at 0.0s,
    // fades to 20% at 1.1s, keyoff fades to 20% in 0.0s.
    { 1397,1397,  0,   1053,     6 }, // 1303: f15GM5; f26GM5; Chorused Piano

    // Amplitude begins at 1635.8, peaks 1789.6 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1398,1398,  0,  40000,     0 }, // 1304: f15GM6; f26GM6; Harpsichord

    // Amplitude begins at 2895.7, peaks 4029.3 at 0.0s,
    // fades to 20% at 0.8s, keyoff fades to 20% in 0.8s.
    { 1399,1399,  0,    786,   786 }, // 1305: f15GM7; f26GM7; Clavinet

    // Amplitude begins at  994.7, peaks 1125.6 at 13.8s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1400,1400,  0,  40000,    73 }, // 1306: f15GM8; f26GM8; Celesta

    // Amplitude begins at 2881.8, peaks 3004.2 at 24.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1401,1401,  0,  40000,    40 }, // 1307: f15GM9; f26GM9; Glockenspiel

    // Amplitude begins at 3632.6,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1402,1402,  0,  40000,    20 }, // 1308: f15GM10; f26GM10; Music box

    // Amplitude begins at   72.8, peaks 1939.0 at 39.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1403,1403,  0,  40000,     0 }, // 1309: f15GM11; f26GM11; Vibraphone

    // Amplitude begins at  336.5, peaks 2147.3 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.5s.
    { 1404,1404,  0,  40000,   466 }, // 1310: f15GM12; f26GM12; Marimba

    // Amplitude begins at   93.0, peaks 2958.0 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 1405,1405,  0,  40000,   193 }, // 1311: f15GM13; f26GM13; Xylophone

    // Amplitude begins at   97.3, peaks 1617.1 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.3s.
    { 1406,1406,  0,  40000,   280 }, // 1312: f15GM14; f26GM14; Tubular Bells

    // Amplitude begins at  521.2, peaks 3745.3 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1407,1407,  0,  40000,     6 }, // 1313: f15GM24; Acoustic Guitar1

    // Amplitude begins at   45.7, peaks 1682.7 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1408,1408,  0,  40000,     6 }, // 1314: f15GM25; f26GM25; Acoustic Guitar2

    // Amplitude begins at 1014.5, peaks 1200.8 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1409,1409,  0,  40000,     0 }, // 1315: f15GM26; f26GM26; Electric Guitar1

    // Amplitude begins at  891.5, peaks 1489.7 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1410,1410,  0,  40000,     0 }, // 1316: f15GM27; f26GM27; Electric Guitar2

    // Amplitude begins at  535.5,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1411,1411,  0,  40000,    20 }, // 1317: f15GM29; f26GM29; Overdrive Guitar

    // Amplitude begins at 1144.2, peaks 1211.9 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1412,1412,  0,  40000,     6 }, // 1318: f15GM31; f26GM31; Guitar Harmonics

    // Amplitude begins at    0.4, peaks 1383.1 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.0s.
    { 1413,1413,  0,  40000,   966 }, // 1319: f15GM32; f26GM32; Acoustic Bass

    // Amplitude begins at    0.0, peaks  702.0 at 37.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 1414,1414,  0,  40000,   240 }, // 1320: f15GM35; f26GM35; Fretless Bass

    // Amplitude begins at 1042.9, peaks 1088.6 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1415,1415,  0,  40000,    33 }, // 1321: f15GM37; f26GM37; Slap Bass 2

    // Amplitude begins at    0.9, peaks 3643.6 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1416,1416,  0,  40000,    13 }, // 1322: f15GM39; f26GM39; Synth Bass 2

    // Amplitude begins at    0.0, peaks 2358.4 at 0.1s,
    // fades to 20% at 1.1s, keyoff fades to 20% in 1.1s.
    { 1417,1417,  0,   1133,  1133 }, // 1323: f15GM41; f26GM41; Viola

    // Amplitude begins at  791.9, peaks 2168.2 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1418,1418,  0,  40000,    13 }, // 1324: f15GM42; f26GM42; Cello

    // Amplitude begins at  874.4, peaks 1004.0 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1419,1419,  0,  40000,    13 }, // 1325: f15GM44; f26GM44; Tremulo Strings

    // Amplitude begins at  115.2, peaks 4111.6 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1420,1420,  0,  40000,    13 }, // 1326: f15GM45; f26GM45; Pizzicato String

    // Amplitude begins at 2284.4, peaks 2364.2 at 0.0s,
    // fades to 20% at 0.8s, keyoff fades to 20% in 0.8s.
    { 1421,1421,  0,    826,   826 }, // 1327: f15GM46; f26GM46; Orchestral Harp

    // Amplitude begins at    2.2, peaks  915.9 at 1.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1422,1422,  0,  40000,    33 }, // 1328: f15GM48; f26GM48; String Ensemble1

    // Amplitude begins at  130.7, peaks 4099.6 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1423,1423,  0,  40000,    20 }, // 1329: f15GM50; f26GM50; Synth Strings 1

    // Amplitude begins at    0.5, peaks 1442.0 at 0.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1424,1424,  0,  40000,   133 }, // 1330: f15GM52; f26GM52; Choir Aahs

    // Amplitude begins at    0.0, peaks  926.0 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1425,1425,  0,  40000,    53 }, // 1331: f15GM53; f26GM53; Voice Oohs

    // Amplitude begins at    0.0, peaks 1229.1 at 0.5s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 1426,1426,  0,  40000,   233 }, // 1332: f15GM54; f26GM54; Synth Voice

    // Amplitude begins at 1825.9, peaks 2766.9 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1427,1427,  0,  40000,     0 }, // 1333: f15GM55; f26GM55; Orchestra Hit

    // Amplitude begins at   94.7, peaks 2890.2 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1428,1428,  0,  40000,   133 }, // 1334: f15GM56; f26GM56; Trumpet

    // Amplitude begins at 2422.8, peaks 2438.7 at 0.1s,
    // fades to 20% at 0.8s, keyoff fades to 20% in 0.8s.
    { 1429,1429,  0,    766,   766 }, // 1335: f15GM59; f26GM59; Muted Trumpet

    // Amplitude begins at 1983.4, peaks 2022.3 at 0.0s,
    // fades to 20% at 1.1s, keyoff fades to 20% in 1.1s.
    { 1430,1430,  0,   1133,  1133 }, // 1336: f15GM60; f26GM60; French Horn

    // Amplitude begins at 1972.6, peaks 2175.6 at 0.0s,
    // fades to 20% at 1.0s, keyoff fades to 20% in 1.0s.
    { 1431,1431,  0,   1046,  1046 }, // 1337: f15GM61; f26GM61; Brass Section

    // Amplitude begins at 1924.8,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1432,1432,  0,  40000,    13 }, // 1338: f15GM62; f26GM62; Synth Brass 1

    // Amplitude begins at  838.2, peaks 1625.2 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.5s.
    { 1433,1433,  0,  40000,   540 }, // 1339: f15GM63; f26GM63; Synth Brass 2

    // Amplitude begins at 2763.8, peaks 3331.7 at 14.8s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1434,1434,  0,  40000,    26 }, // 1340: f15GM66; f26GM66; Tenor Sax

    // Amplitude begins at 3353.7, peaks 4522.8 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1435,1435,  0,  40000,    20 }, // 1341: f15GM67; f26GM67; Baritone Sax

    // Amplitude begins at 1984.4, peaks 2186.1 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1436,1436,  0,  40000,     0 }, // 1342: f15GM68; f26GM68; Oboe

    // Amplitude begins at 1068.8, peaks 1225.1 at 0.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1437,1437,  0,  40000,     0 }, // 1343: f15GM69; f26GM69; English Horn

    // Amplitude begins at  848.9, peaks 2902.1 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1438,1438,  0,  40000,     0 }, // 1344: f15GM77; f26GM77; Shakuhachi

    // Amplitude begins at  709.7, peaks 2589.2 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1439,1439,  0,  40000,     0 }, // 1345: f15GM78; f26GM78; Whistle

    // Amplitude begins at  941.6, peaks 2573.9 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1440,1440,  0,  40000,     0 }, // 1346: f15GM79; f26GM79; Ocarina

    // Amplitude begins at   78.6, peaks 1421.2 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1441,1441,  0,  40000,     0 }, // 1347: f15GM80; f26GM80; Lead 1 squareea

    // Amplitude begins at 1331.1, peaks 1677.8 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1442,1442,  0,  40000,    93 }, // 1348: f15GM81; f26GM81; Lead 2 sawtooth

    // Amplitude begins at    0.3, peaks 1248.4 at 18.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1443,1443,  0,  40000,    73 }, // 1349: f15GM87; f26GM87; Lead 8 brass

    // Amplitude begins at    3.0, peaks 1213.0 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1444,1444,  0,  40000,     0 }, // 1350: f15GM88; Pad 1 new age

    // Amplitude begins at    3.0, peaks 1377.7 at 24.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1445,1445,  0,  40000,     0 }, // 1351: f15GM89; f26GM89; Pad 2 warm

    // Amplitude begins at 2512.3, peaks 3121.6 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1446,1446,  0,  40000,     0 }, // 1352: f15GM90; f26GM90; Pad 3 polysynth

    // Amplitude begins at 1255.5, peaks 3050.1 at 16.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1447,1447,  0,  40000,     0 }, // 1353: f15GM91; f26GM91; Pad 4 choir

    // Amplitude begins at 1749.6, peaks 4658.6 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1448,1448,  0,  40000,     6 }, // 1354: f15GM92; Pad 5 bowedpad

    // Amplitude begins at 1746.2, peaks 2528.1 at 5.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1449,1449,  0,  40000,     0 }, // 1355: f15GM93; Pad 6 metallic

    // Amplitude begins at  493.8, peaks 1167.1 at 17.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1450,1450,  0,  40000,     0 }, // 1356: f15GM94; f26GM94; Pad 7 halo

    // Amplitude begins at    4.1, peaks 1586.3 at 6.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1451,1451,  0,  40000,     0 }, // 1357: f15GM95; f26GM95; Pad 8 sweep

    // Amplitude begins at    4.8, peaks 1842.2 at 1.6s,
    // fades to 20% at 1.7s, keyoff fades to 20% in 1.7s.
    { 1452,1452,  0,   1686,  1686 }, // 1358: f15GM96; f26GM96; FX 1 rain

    // Amplitude begins at 2203.9, peaks 2354.6 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1453,1453,  0,  40000,   106 }, // 1359: f15GM98; f26GM98; FX 3 crystal

    // Amplitude begins at 1392.1, peaks 2405.8 at 0.0s,
    // fades to 20% at 0.8s, keyoff fades to 20% in 0.8s.
    { 1454,1454,  0,    753,   753 }, // 1360: f15GM99; f26GM99; FX 4 atmosphere

    // Amplitude begins at 2457.8, peaks 3463.0 at 0.0s,
    // fades to 20% at 1.5s, keyoff fades to 20% in 1.5s.
    { 1455,1455,  0,   1500,  1500 }, // 1361: f15GM100; f26GM100; FX 5 brightness

    // Amplitude begins at 2471.8, peaks 2819.6 at 0.0s,
    // fades to 20% at 1.9s, keyoff fades to 20% in 1.9s.
    { 1456,1456,  0,   1900,  1900 }, // 1362: f15GM101; f26GM101; FX 6 goblins

    // Amplitude begins at 1812.3, peaks 2785.4 at 0.1s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 1457,1457,  0,    333,   333 }, // 1363: f15GM102; f26GM102; FX 7 echoes

    // Amplitude begins at 1016.0,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1458,1458,  0,    140,   140 }, // 1364: f15GM103; f26GM103; FX 8 sci-fi

    // Amplitude begins at 1934.1,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 1459,1459,  0,    160,   160 }, // 1365: f15GM104; f26GM104; Sitar

    // Amplitude begins at 2510.0,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 1460,1460,  0,    600,   600 }, // 1366: f15GM105; f26GM105; Banjo

    // Amplitude begins at    0.0, peaks 2393.8 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1461,1461,  0,  40000,    46 }, // 1367: f15GM108; f26GM108; Kalimba

    // Amplitude begins at    0.6, peaks 2928.5 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1462,1462,  0,  40000,    46 }, // 1368: f15GM109; f26GM109; Bagpipe

    // Amplitude begins at 1242.3, peaks 2087.4 at 11.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1463,1463,  0,  40000,   106 }, // 1369: f15GM110; f26GM110; Fiddle

    // Amplitude begins at  362.0, peaks 3090.1 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1464,1464,  0,  40000,     0 }, // 1370: f15GM111; f26GM111; Shanai

    // Amplitude begins at 2735.9, peaks 3010.3 at 0.0s,
    // fades to 20% at 0.8s, keyoff fades to 20% in 0.8s.
    { 1465,1465,  0,    820,   820 }, // 1371: f15GM112; f26GM112; Tinkle Bell

    // Amplitude begins at 2381.2,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 1466,1466,  0,    433,   433 }, // 1372: f15GM113; f26GM113; Agogo Bells

    // Amplitude begins at  923.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1467,1467,  0,     40,    40 }, // 1373: f15GM114; f26GM114; Steel Drums

    // Amplitude begins at 1095.9,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1468,1468,  0,     40,    40 }, // 1374: f15GM115; f26GM115; Woodblock

    // Amplitude begins at 1625.1, peaks 2813.2 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1469,1469,  0,    146,   146 }, // 1375: f15GM116; f26GM116; Taiko Drum

    // Amplitude begins at  679.3,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1470,1470,  0,     40,    40 }, // 1376: f15GM117; f26GM117; Melodic Tom

    // Amplitude begins at 1447.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1471,1471,  0,     13,    13 }, // 1377: f15GM118; f26GM118; Synth Drum

    // Amplitude begins at  367.7, peaks  371.6 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 1472,1472,  0,    306,   306 }, // 1378: f15GM119; f26GM119; Reverse Cymbal

    // Amplitude begins at 2355.6,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1473,1473,  0,     40,    40 }, // 1379: f15GM120; f26GM120; Guitar FretNoise

    // Amplitude begins at  492.7,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 1474,1474,  0,    566,   566 }, // 1380: f15GM121; f26GM121; Breath Noise

    // Amplitude begins at  749.6, peaks 1292.1 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 1475,1475,  0,    286,   286 }, // 1381: f15GM122; f26GM122; Seashore

    // Amplitude begins at 1105.4, peaks 1230.8 at 0.0s,
    // fades to 20% at 2.4s, keyoff fades to 20% in 2.4s.
    { 1476,1476,  0,   2406,  2406 }, // 1382: f15GM123; f26GM123; Bird Tweet

    // Amplitude begins at  726.4,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1477,1477,  0,     80,    80 }, // 1383: f15GM125; f26GM125; Helicopter

    // Amplitude begins at 1817.8, peaks 2026.3 at 0.1s,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    { 1478,1478,  0,   1233,  1233 }, // 1384: f15GM126; f26GM126; Applause/Noise

    // Amplitude begins at    3.8, peaks 1479.9 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 1479,1479,  0,    300,   300 }, // 1385: f15GM127; f26GM127; Gunshot

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1480,1480,  0,      0,     0 }, // 1386: f15GP0; f15GP1; f15GP10; f15GP101; f15GP102; f15GP103; f15GP104; f15GP105; f15GP106; f15GP107; f15GP108; f15GP109; f15GP11; f15GP110; f15GP111; f15GP112; f15GP113; f15GP114; f15GP115; f15GP116; f15GP117; f15GP118; f15GP119; f15GP12; f15GP120; f15GP121; f15GP122; f15GP123; f15GP124; f15GP125; f15GP126; f15GP127; f15GP13; f15GP14; f15GP15; f15GP16; f15GP17; f15GP18; f15GP19; f15GP2; f15GP20; f15GP21; f15GP22; f15GP23; f15GP24; f15GP25; f15GP26; f15GP27; f15GP28; f15GP29; f15GP3; f15GP30; f15GP31; f15GP32; f15GP33; f15GP34; f15GP4; f15GP5; f15GP52; f15GP53; f15GP55; f15GP57; f15GP58; f15GP59; f15GP6; f15GP7; f15GP74; f15GP76; f15GP77; f15GP78; f15GP79; f15GP8; f15GP80; f15GP81; f15GP82; f15GP83; f15GP84; f15GP85; f15GP86; f15GP87; f15GP88; f15GP89; f15GP9; f15GP90; f15GP91; f15GP92; f15GP93; f15GP94; f15GP95; f15GP96; f15GP97; f15GP98; f15GP99; f26GP0; f26GP1; f26GP10; f26GP101; f26GP102; f26GP103; f26GP104; f26GP105; f26GP106; f26GP107; f26GP108; f26GP109; f26GP11; f26GP110; f26GP111; f26GP112; f26GP113; f26GP114; f26GP115; f26GP116; f26GP117; f26GP118; f26GP119; f26GP12; f26GP120; f26GP121; f26GP122; f26GP123; f26GP124; f26GP125; f26GP126; f26GP127; f26GP13; f26GP14; f26GP15; f26GP16; f26GP17; f26GP18; f26GP19; f26GP2; f26GP20; f26GP21; f26GP22; f26GP23; f26GP24; f26GP25; f26GP26; f26GP27; f26GP28; f26GP29; f26GP3; f26GP30; f26GP31; f26GP32; f26GP33; f26GP34; f26GP4; f26GP5; f26GP52; f26GP53; f26GP55; f26GP57; f26GP58; f26GP59; f26GP6; f26GP7; f26GP74; f26GP76; f26GP77; f26GP78; f26GP79; f26GP8; f26GP80; f26GP81; f26GP82; f26GP83; f26GP84; f26GP85; f26GP86; f26GP87; f26GP88; f26GP89; f26GP9; f26GP90; f26GP91; f26GP92; f26GP93; f26GP94; f26GP95; f26GP96; f26GP97; f26GP98; f26GP99; Bell Tree; Castanets; Chinese Cymbal; Crash Cymbal 2; High Wood Block; Jingle Bell; Long Guiro; Low Wood Block; Mute Cuica; Mute Surdu; Mute Triangle; Open Cuica; Open Surdu; Open Triangle; Ride Bell; Ride Cymbal 2; Shaker; Splash Cymbal; Vibraslap

    // Amplitude begins at 1373.1, peaks 1384.7 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1481,1481, 17,     33,    33 }, // 1387: f15GP35; f15GP36; f26GP35; f26GP36; Ac Bass Drum; Bass Drum 1

    // Amplitude begins at  610.5,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1482,1482,  1,     13,    13 }, // 1388: f15GP37; f26GP37; Side Stick

    // Amplitude begins at  457.8,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1483,1483, 33,     93,    93 }, // 1389: f15GP38; f26GP38; Acoustic Snare

    // Amplitude begins at 1475.2,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1471,1471, 16,      6,     6 }, // 1390: f15GP39; f26GP39; Hand Clap

    // Amplitude begins at  369.1,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1484,1484, 32,    100,   100 }, // 1391: f15GP40; f26GP40; Electric Snare

    // Amplitude begins at 1718.8, peaks 2066.2 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 261,261,  0,     33,    33 }, // 1392: f15GP41; f15GP43; f15GP45; f15GP47; f15GP48; f15GP50; f26GP41; f26GP43; f26GP45; f26GP47; f26GP48; f26GP50; High Floor Tom; High Tom; High-Mid Tom; Low Floor Tom; Low Tom; Low-Mid Tom

    // Amplitude begins at  130.3,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1485,1485, 12,     60,    60 }, // 1393: f15GP42; f26GP42; Closed High Hat

    // Amplitude begins at 1171.7,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1486,1486, 17,      6,     6 }, // 1394: f15GP56; f26GP56; Cow Bell

    // Amplitude begins at 1837.8,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1487,1487,  1,     13,    13 }, // 1395: f15GP62; f26GP62; Mute High Conga

    // Amplitude begins at  774.5, peaks  828.9 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1488,1488,  1,     33,    33 }, // 1396: f15GP65; f26GP65; High Timbale

    // Amplitude begins at  726.4,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1489,1489,  0,     80,    80 }, // 1397: f15GP66; f26GP66; Low Timbale

    // Amplitude begins at  716.9,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1490,1490,  3,     46,    46 }, // 1398: f15GP67; f26GP67; High Agogo

    // Amplitude begins at  742.0, peaks  980.6 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1491,1491,  3,     46,    46 }, // 1399: f15GP68; f26GP68; Low Agogo

    // Amplitude begins at 2874.4,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1492,1492, 17,     40,    40 }, // 1400: f15GP73; f26GP73; Short Guiro

    // Amplitude begins at    0.0, peaks  389.4 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1493,1493,  0,  40000,    40 }, // 1401: f16GM41; f37GM41; f54GM41; Viola

    // Amplitude begins at 1012.8, peaks 1076.5 at 0.0s,
    // fades to 20% at 1.9s, keyoff fades to 20% in 1.9s.
    { 1494,1494,  0,   1886,  1886 }, // 1402: f16GM46; f37GM46; f54GM46; Orchestral Harp

    // Amplitude begins at  561.8, peaks 1161.5 at 31.5s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.3s.
    { 1495,1496,  0,  40000,   266 }, // 1403: f16GM48; f54GM48; String Ensemble1

    // Amplitude begins at  290.5, peaks 3297.3 at 0.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1497,1498,  0,  40000,    66 }, // 1404: f16GM56; f54GM56; Trumpet

    // Amplitude begins at    1.7, peaks 3851.0 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1499,1500,  0,  40000,    33 }, // 1405: f16GM58; f54GM58; Tuba

    // Amplitude begins at  290.7, peaks 5062.6 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1501,1502,  0,  40000,    53 }, // 1406: f16GM60; f54GM60; French Horn

    // Amplitude begins at 1772.5, peaks 1901.4 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1503,1503,  0,     20,    20 }, // 1407: f16GP35; Ac Bass Drum

    // Amplitude begins at   13.4, peaks  339.1 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1504,1504, 15,     40,    40 }, // 1408: f16GP70; Maracas

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1505,1505,144,      0,     0 }, // 1409: f16GP73; Short Guiro

    // Amplitude begins at    0.0, peaks  692.9 at 0.1s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 1506,1506, 16,    160,   160 }, // 1410: f16GP74; Long Guiro

    // Amplitude begins at 1105.8, peaks 1739.2 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1507,1507,  0,     40,    40 }, // 1411: f16GP87; Open Surdu

    // Amplitude begins at    0.0, peaks  452.7 at 2.2s,
    // fades to 20% at 2.3s, keyoff fades to 20% in 2.3s.
    { 1508,1508,  0,   2333,  2333 }, // 1412: f17GM119; f35GM119; Reverse Cymbal

    // Amplitude begins at 1044.9, peaks 1206.5 at 0.0s,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.9s.
    { 1509,1509,  0,    906,   906 }, // 1413: b41M2; f19GM2; f21GM2; f41GM2; ElecGrandPiano; elecvibe

    // Amplitude begins at 1776.7, peaks 2633.1 at 14.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1510,1510,  0,  40000,    40 }, // 1414: b41M14; b41M3; f19GM14; f19GM3; f21GM14; f41GM14; Honky-tonkPiano; Tubular Bells; pipes.in

    // Amplitude begins at  367.5, peaks  866.7 at 0.0s,
    // fades to 20% at 7.6s, keyoff fades to 20% in 7.6s.
    { 1511,1511,  0,   7553,  7553 }, // 1415: b41M4; f19GM4; Rhodes Piano; circus.i

    // Amplitude begins at 1187.8,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 1512,1512,  0,    260,   260 }, // 1416: b41M8; f19GM8; f21GM8; f41GM8; Celesta; SB8.ins

    // Amplitude begins at 2171.6, peaks 2223.1 at 31.7s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1513,1513,  0,  40000,     0 }, // 1417: b41M10; f19GM10; f41GM10; 60sorgan; Music box

    // Amplitude begins at 2501.7, peaks 3173.6 at 2.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1514,1514,  0,  40000,     6 }, // 1418: f19GM11; Vibraphone

    // Amplitude begins at 2252.4, peaks 2422.5 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 1515,1515,  0,    206,   206 }, // 1419: b41M12; f19GM12; f21GM12; f41GM12; Marimba; SB12.ins

    // Amplitude begins at 1463.7, peaks 2044.1 at 0.0s,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 1516,1516,  0,    380,   380 }, // 1420: f19GM13; Xylophone

    // Amplitude begins at    0.0, peaks  914.9 at 15.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1517,1517,  0,  40000,     6 }, // 1421: b41M15; f19GM15; f21GM15; f41GM15; Dulcimer; pirate.i

    // Amplitude begins at 2033.2,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1518,1518,  0,     20,    20 }, // 1422: b41M19; f19GM19; f21GM19; f41GM19; Church Organ; logdrum1

    // Amplitude begins at   29.8, peaks  801.8 at 27.7s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1519,1519,  0,  40000,   100 }, // 1423: f19GM21; Accordion

    // Amplitude begins at 2252.4, peaks 2422.5 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 1520,1520,  0,    206,   206 }, // 1424: b41M24; f19GM24; f21GM24; f41GM24; Acoustic Guitar1; SB24.ins

    // Amplitude begins at  454.4, peaks  508.6 at 37.8s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1521,1521,  0,  40000,     6 }, // 1425: f19GM25; f41GM25; Acoustic Guitar2

    // Amplitude begins at 1124.0, peaks 1198.2 at 0.0s,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 1522,1522,  0,    400,   400 }, // 1426: b41M26; f19GM26; f21GM26; f41GM26; Electric Guitar1; SB26.ins

    // Amplitude begins at 2362.0, peaks 2692.4 at 0.0s,
    // fades to 20% at 0.5s, keyoff fades to 20% in 0.5s.
    { 1523,1523,  0,    466,   466 }, // 1427: b41M32; f19GM32; f21GM32; f41GM32; Acoustic Bass; SB32.ins

    // Amplitude begins at    0.0, peaks 1496.9 at 2.3s,
    // fades to 20% at 2.7s, keyoff fades to 20% in 2.7s.
    { 1524,1524,  0,   2720,  2720 }, // 1428: f19GM33; f19GM36; Electric Bass 1; Slap Bass 1

    // Amplitude begins at 3212.7, peaks 3495.3 at 0.0s,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    { 1525,1525,  0,   1160,  1160 }, // 1429: b41M38; f19GM38; f41GM38; Synth Bass 1; trainbel

    // Amplitude begins at   74.0, peaks 1698.2 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1526,1526,  0,  40000,    20 }, // 1430: b41M41; f19GM41; f21GM41; f41GM41; SB41.ins; Viola

    // Amplitude begins at   33.3, peaks  909.7 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1527,1527,  0,  40000,    40 }, // 1431: f19GM43; Contrabass

    // Amplitude begins at   99.2, peaks 3576.2 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.9s.
    { 1528,1528,  0,  40000,   946 }, // 1432: f19GM48; f19GM50; String Ensemble1; Synth Strings 1

    // Amplitude begins at 1162.8, peaks 2262.6 at 0.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.3s.
    { 1529,1529,  0,  40000,   300 }, // 1433: b41M94; f19GM49; f19GM94; f41GM94; Pad 7 halo; SB94.ins; String Ensemble2

    // Amplitude begins at  791.1,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1530,1530,  0,     80,    80 }, // 1434: b41M105; b41M51; f19GM105; f19GM51; f21GM105; f41GM105; f41GM51; Banjo; SynthStrings 2; koto1.in

    // Amplitude begins at  664.5, peaks 1749.0 at 6.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1531,1531,  0,  40000,     6 }, // 1435: b41M71; f19GM71; f41GM71; Clarinet; SB71.ins

    // Amplitude begins at    7.4, peaks 5492.7 at 0.1s,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.9s.
    { 1532,1532,  0,    906,   906 }, // 1436: b41M72; b41M73; b41M74; b41M75; f19GM72; f19GM73; f19GM74; f19GM75; f21GM72; f21GM75; f41GM72; f41GM73; f41GM74; f41GM75; Flute; Pan Flute; Piccolo; Recorder; flute1.i

    // Amplitude begins at 1847.7,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1533,1533,  0,     13,    13 }, // 1437: b41M76; b41M78; f19GM76; f41GM76; Bottle Blow; cowboy2.

    // Amplitude begins at  387.2, peaks 1198.3 at 28.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1534,1534,  0,  40000,    66 }, // 1438: f19GM77; Shakuhachi

    // Amplitude begins at 1002.4, peaks 1099.3 at 29.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1535,1535,  0,  40000,     6 }, // 1439: b41M81; f19GM81; f27GM81; f41GM81; Lead 2 sawtooth; SB81.ins

    // Amplitude begins at 2235.1, peaks 3119.6 at 37.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1536,1536,  0,  40000,     0 }, // 1440: b41M82; f19GM82; f41GM82; Lead 3 calliope; airplane

    // Amplitude begins at 2489.8, peaks 3124.2 at 15.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.4s.
    { 1537,1537,  0,  40000,   400 }, // 1441: b41M87; f19GM87; Lead 8 brass; harmonca

    // Amplitude begins at    6.7, peaks 3755.1 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1538,1538,  0,  40000,    53 }, // 1442: f19GM107; f19GM108; f19GM109; f19GM93; Bagpipe; Kalimba; Koto; Pad 6 metallic

    // Amplitude begins at 2089.3, peaks 2518.4 at 0.0s,
    // fades to 20% at 0.7s, keyoff fades to 20% in 0.7s.
    { 1539,1539,  0,    733,   733 }, // 1443: b41M96; f19GM96; FX 1 rain; SB96.ins

    // Amplitude begins at 1896.7, peaks 2552.0 at 25.8s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1540,1540,  0,  40000,     6 }, // 1444: b41M97; f19GM97; f21GM97; f41GM97; FX 2 soundtrack; organ3a.

    // Amplitude begins at  292.1, peaks  361.9 at 0.0s,
    // fades to 20% at 1.8s, keyoff fades to 20% in 1.8s.
    { 1541,1541,  0,   1800,  1800 }, // 1445: f19GM100; FX 5 brightness

    // Amplitude begins at  413.8, peaks  472.2 at 27.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 1542,1542,  0,  40000,   193 }, // 1446: f19GM103; FX 8 sci-fi

    // Amplitude begins at  578.2, peaks  707.5 at 0.0s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 1543,1543,  0,    606,   606 }, // 1447: b41M113; f19GM113; f21GM113; f41GM113; Agogo Bells; SB113.in

    // Amplitude begins at 2746.1,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1544,1544,  0,    120,   120 }, // 1448: b41M115; f19GM115; SB115.in; Woodblock

    // Amplitude begins at   38.6, peaks  922.6 at 6.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.6s.
    { 1545,1545,  0,  40000,   573 }, // 1449: b41M124; f19GM124; f41GM124; Telephone; chirp.in

    // Amplitude begins at    0.0, peaks 1782.6 at 2.3s,
    // fades to 20% at 4.6s, keyoff fades to 20% in 4.6s.
    { 1546,1546,  0,   4566,  4566 }, // 1450: b41M125; f19GM125; f21GM125; f41GM125; Helicopter; SB125.in

    // Amplitude begins at  813.1,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1547,1547,  0,     80,    80 }, // 1451: f19GM126; f21GM126; f27GM127; f41GM126; Applause/Noise; Gunshot

    // Amplitude begins at 1620.5, peaks 2113.4 at 2.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 3.3s.
    { 1548,1548,  0,  40000,  3293 }, // 1452: f19GM127; f21GM127; f41GM127; Gunshot

    // Amplitude begins at 2664.9, peaks 3706.4 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1549,1549,  0,     40,    40 }, // 1453: f19GP35; f19GP36; f27GP36; f41GP36; Ac Bass Drum; Bass Drum 1

    // Amplitude begins at  841.4,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 1547,1547, 30,    160,   160 }, // 1454: f19GP38; f19GP60; f21GP60; f27GP38; f27GP39; f27GP40; f41GP38; Acoustic Snare; Electric Snare; Hand Clap; High Bongo

    // Amplitude begins at  295.5, peaks  400.2 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1550,1550,100,     33,    33 }, // 1455: f19GP42; Closed High Hat

    // Amplitude begins at 1874.6, peaks 2558.0 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1551,1551,  2,     46,    46 }, // 1456: f19GP48; f19GP52; f19GP53; f21GP48; f21GP52; f21GP53; f41GP48; f41GP52; f41GP53; Chinese Cymbal; High-Mid Tom; Ride Bell

    // Amplitude begins at 1293.6,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1552,1552, 19,     40,    40 }, // 1457: f19GP49; f19GP63; f19GP67; f19GP68; f21GP49; f21GP67; f21GP68; f27GP32; f27GP33; f27GP34; f27GP37; f27GP67; f27GP68; f27GP75; f27GP85; f41GP49; f41GP60; f41GP67; f41GP68; Castanets; Claves; Crash Cymbal 1; High Agogo; High Bongo; Low Agogo; Open High Conga; Side Stick

    // Amplitude begins at 1628.4,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 1553,1554, 15,    273,   273 }, // 1458: f20GP59; Ride Cymbal 2

    // Amplitude begins at   29.6, peaks  801.0 at 22.7s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.1s.
    { 1555,1555,  0,  40000,  1060 }, // 1459: f21GM3; Honky-tonkPiano

    // Amplitude begins at 1454.7, peaks 1682.2 at 0.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.4s.
    { 1556,1556,  0,  40000,   440 }, // 1460: f21GM4; Rhodes Piano

    // Amplitude begins at  848.5, peaks  931.4 at 0.1s,
    // fades to 20% at 1.1s, keyoff fades to 20% in 1.1s.
    { 1557,1557,  0,   1093,  1093 }, // 1461: f21GM5; f41GM5; Chorused Piano

    // Amplitude begins at 1643.0, peaks 1817.6 at 0.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.3s.
    { 1558,1558,  0,  40000,   293 }, // 1462: f21GM9; Glockenspiel

    // Amplitude begins at    0.0, peaks 3174.4 at 0.5s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.9s.
    { 1559,1559,  0,  40000,   893 }, // 1463: f21GM10; Music box

    // Amplitude begins at 2506.5, peaks 3174.3 at 36.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1560,1560,  0,  40000,     6 }, // 1464: b41M11; f21GM11; Vibraphone; organ4.i

    // Amplitude begins at 1459.5, peaks 2045.3 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 1561,1561,  0,    346,   346 }, // 1465: b41M13; f21GM13; f41GM13; SB13.ins; Xylophone

    // Amplitude begins at 3437.2,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1562,1562,  0,  40000,     0 }, // 1466: b41M16; f21GM16; f41GM16; Hammond Organ; harpsi7.

    // Amplitude begins at  564.4,
    // fades to 20% at 1.0s, keyoff fades to 20% in 1.0s.
    { 1563,1563,  0,   1013,  1013 }, // 1467: b41M17; f21GM17; f27GM6; f41GM17; Harpsichord; Percussive Organ; harpsi6.

    // Amplitude begins at   29.8, peaks  800.3 at 23.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1564,1564,  0,  40000,   100 }, // 1468: b41M21; f21GM21; f41GM21; Accordion; whistle.

    // Amplitude begins at  811.8, peaks 3291.6 at 5.7s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1565,1565,  0,  40000,     0 }, // 1469: b41M22; f21GM22; f21GM54; f41GM22; Harmonica; Synth Voice; arabian2

    // Amplitude begins at  948.0, peaks 3409.1 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1566,1566,  0,  40000,     0 }, // 1470: b41M23; f21GM23; f27GM68; f41GM23; Oboe; Tango Accordion; arabian.

    // Amplitude begins at  113.0, peaks 2782.2 at 32.7s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1567,1567,  0,  40000,   126 }, // 1471: b41M25; f21GM25; Acoustic Guitar2; whistle2

    // Amplitude begins at 2514.5,
    // fades to 20% at 1.4s, keyoff fades to 20% in 1.4s.
    { 1568,1568,  0,   1386,  1386 }, // 1472: f21GM27; Electric Guitar2

    // Amplitude begins at 2030.8,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.3s.
    { 1569,1569,  0,  40000,   300 }, // 1473: f21GM28; f41GM28; Electric Guitar3

    // Amplitude begins at    6.8, peaks 4480.1 at 15.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.5s.
    { 1570,1570,  0,  40000,   513 }, // 1474: f21GM29; Overdrive Guitar

    // Amplitude begins at    0.0, peaks 1496.9 at 2.3s,
    // fades to 20% at 4.1s, keyoff fades to 20% in 4.1s.
    { 1571,1571,  0,   4053,  4053 }, // 1475: f21GM33; f41GM33; f41GM36; Electric Bass 1; Slap Bass 1

    // Amplitude begins at 1438.4, peaks 1675.5 at 0.7s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1572,1572,  0,  40000,     6 }, // 1476: f21GM34; Electric Bass 2

    // Amplitude begins at 2220.8,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1573,1573,  0,  40000,    13 }, // 1477: f21GM35; Fretless Bass

    // Amplitude begins at  865.2, peaks  872.0 at 22.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1574,1574,  0,  40000,    20 }, // 1478: f21GM36; Slap Bass 1

    // Amplitude begins at    0.0, peaks 1230.9 at 39.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.2s.
    { 1575,1575,  0,  40000,  1173 }, // 1479: f21GM37; Slap Bass 2

    // Amplitude begins at    0.0, peaks 1081.1 at 0.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1576,1576,  0,  40000,    20 }, // 1480: f21GM38; Synth Bass 1

    // Amplitude begins at 1446.9, peaks 1676.9 at 0.7s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1577,1577,  0,  40000,     6 }, // 1481: f21GM39; Synth Bass 2

    // Amplitude begins at 1453.7, peaks 1676.4 at 0.7s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1578,1578,  0,  40000,     6 }, // 1482: f21GM40; Violin

    // Amplitude begins at    0.7, peaks 4087.8 at 39.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.5s.
    { 1579,1579,  0,  40000,   473 }, // 1483: f21GM42; f41GM42; Cello

    // Amplitude begins at   33.1, peaks  831.7 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1580,1580,  0,  40000,    33 }, // 1484: b41M43; f21GM43; f41GM43; Contrabass; SB43.ins

    // Amplitude begins at 2220.8,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1581,1581,  0,  40000,     6 }, // 1485: f21GM44; Tremulo Strings

    // Amplitude begins at 1462.3, peaks 1675.9 at 0.7s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1582,1582,  0,  40000,     6 }, // 1486: f21GM45; Pizzicato String

    // Amplitude begins at 1448.4, peaks 1675.5 at 0.7s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1583,1583,  0,  40000,     6 }, // 1487: f21GM46; Orchestral Harp

    // Amplitude begins at 1429.3, peaks 1572.9 at 0.4s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.0s.
    { 1584,1584,  0,    580,    26 }, // 1488: f21GM47; Timpany

    // Amplitude begins at    3.2, peaks 1147.5 at 0.0s,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.9s.
    { 1585,1585,  0,    860,   860 }, // 1489: f21GM48; String Ensemble1

    // Amplitude begins at 1981.2, peaks 2669.4 at 28.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.6s.
    { 1586,1586,  0,  40000,   613 }, // 1490: b41M49; f21GM49; f41GM49; String Ensemble2; strnlong

    // Amplitude begins at    3.6, peaks 3482.7 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.6s.
    { 1587,1587,  0,  40000,  1553 }, // 1491: f21GM50; f41GM48; f41GM50; String Ensemble1; Synth Strings 1

    // Amplitude begins at  864.7, peaks  915.9 at 0.0s,
    // fades to 20% at 0.5s, keyoff fades to 20% in 0.5s.
    { 1588,1588,  0,    486,   486 }, // 1492: f21GM51; SynthStrings 2

    // Amplitude begins at  459.6, peaks 1615.6 at 0.5s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1589,1589,  0,  40000,     0 }, // 1493: f21GM52; Choir Aahs

    // Amplitude begins at  433.2, peaks 1977.4 at 36.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1590,1590,  0,  40000,     0 }, // 1494: f21GM53; Voice Oohs

    // Amplitude begins at    0.3, peaks 4444.9 at 0.3s,
    // fades to 20% at 1.1s, keyoff fades to 20% in 1.1s.
    { 1591,1591,  0,   1126,  1126 }, // 1495: b41M55; f21GM55; f32GM55; f41GM55; Orchestra Hit; cello.in

    // Amplitude begins at    1.7, peaks 2441.8 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1592,1592,  0,  40000,    53 }, // 1496: f21GM61; f21GM88; f41GM88; Brass Section; Pad 1 new age

    // Amplitude begins at 1084.9, peaks 1360.7 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1593,1593,  0,  40000,     0 }, // 1497: b41M62; f21GM62; f27GM30; f41GM62; Distorton Guitar; Synth Brass 1; elecgtr.

    // Amplitude begins at   75.0, peaks 1773.3 at 31.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.5s.
    { 1594,1594,  0,  40000,   473 }, // 1498: f21GM63; Synth Brass 2

    // Amplitude begins at 1210.9, peaks 1357.2 at 0.5s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1595,1595,  0,  40000,     6 }, // 1499: f21GM64; Soprano Sax

    // Amplitude begins at 1210.7, peaks 1355.9 at 0.5s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1596,1596,  0,  40000,     6 }, // 1500: f21GM65; Alto Sax

    // Amplitude begins at 1177.2, peaks 1417.9 at 0.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1597,1597,  0,  40000,     6 }, // 1501: f21GM66; Tenor Sax

    // Amplitude begins at 1212.7, peaks 1379.3 at 0.5s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1598,1598,  0,  40000,     6 }, // 1502: f21GM67; Baritone Sax

    // Amplitude begins at 1204.7, peaks 1368.8 at 0.5s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1599,1599,  0,  40000,     6 }, // 1503: f21GM68; Oboe

    // Amplitude begins at 1438.4, peaks 1675.5 at 0.7s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1600,1600,  0,  40000,     6 }, // 1504: f21GM69; English Horn

    // Amplitude begins at    0.8, peaks 4704.1 at 1.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1601,1601,  0,  40000,    40 }, // 1505: f21GM70; Bassoon

    // Amplitude begins at  320.1, peaks 1226.2 at 33.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.6s.
    { 1602,1602,  0,  40000,   560 }, // 1506: f21GM71; Clarinet

    // Amplitude begins at    7.5, peaks 5845.4 at 36.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1603,1603,  0,  40000,   126 }, // 1507: f21GM73; Flute

    // Amplitude begins at    0.6, peaks 2461.6 at 39.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1604,1604,  0,  40000,   140 }, // 1508: f21GM74; Recorder

    // Amplitude begins at  965.1, peaks 2432.1 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.6s.
    { 1605,1605,  0,  40000,   593 }, // 1509: f21GM76; Bottle Blow

    // Amplitude begins at  387.0, peaks 1224.0 at 11.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1606,1606,  0,  40000,    73 }, // 1510: b41M77; f21GM77; f41GM77; Shakuhachi; afroflut

    // Amplitude begins at 1582.6, peaks 2464.1 at 0.0s,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    { 1607,1607,  0,   1220,  1220 }, // 1511: f21GM78; Whistle

    // Amplitude begins at 1168.5, peaks 1171.8 at 0.0s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 1608,1608,  0,    580,   580 }, // 1512: f21GM79; Ocarina

    // Amplitude begins at  114.5, peaks 4709.0 at 6.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1609,1609,  0,  40000,    86 }, // 1513: f21GM80; Lead 1 squareea

    // Amplitude begins at 2453.0, peaks 2639.6 at 17.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1610,1610,  0,  40000,   140 }, // 1514: f21GM81; f21GM87; f41GM87; Lead 2 sawtooth; Lead 8 brass

    // Amplitude begins at  872.6, peaks 2639.6 at 17.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 2.1s.
    { 1611,1611,  0,  40000,  2140 }, // 1515: f21GM82; Lead 3 calliope

    // Amplitude begins at  634.9, peaks 1125.4 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1612,1612,  0,  40000,     0 }, // 1516: b41M83; f21GM83; f32GM83; f37GM71; f41GM83; f47GM71; Clarinet; Lead 4 chiff; clarinet

    // Amplitude begins at  113.7, peaks 3384.3 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1613,1613,  0,  40000,     0 }, // 1517: b41M84; b41M85; f21GM84; f21GM85; f32GM84; f32GM85; f41GM84; f41GM85; Lead 5 charang; Lead 6 voice; oboe.ins

    // Amplitude begins at 2049.4, peaks 2125.3 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1614,1614,  0,  40000,     6 }, // 1518: b41M86; f21GM86; f32GM86; f41GM86; Lead 7 fifths; bassoon.

    // Amplitude begins at 2465.6, peaks 3173.7 at 35.8s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 3.6s.
    { 1615,1615,  0,  40000,  3626 }, // 1519: f21GM89; f21GM90; f21GM91; f21GM92; f21GM93; Pad 2 warm; Pad 3 polysynth; Pad 4 choir; Pad 5 bowedpad; Pad 6 metallic

    // Amplitude begins at   69.9, peaks 6026.8 at 33.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.2s.
    { 1616,1616,  0,  40000,  1213 }, // 1520: f21GM94; Pad 7 halo

    // Amplitude begins at   35.9, peaks  860.2 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1617,1617,  0,  40000,   126 }, // 1521: b41M95; f21GM95; f41GM95; Pad 8 sweep; brass1.i

    // Amplitude begins at 2089.3, peaks 2518.4 at 0.0s,
    // fades to 20% at 0.7s, keyoff fades to 20% in 0.7s.
    { 1618,1618,  0,    733,   733 }, // 1522: f21GM96; FX 1 rain

    // Amplitude begins at 1221.9,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.9s.
    { 1619,1619,  0,    906,   906 }, // 1523: f21GM98; f32GM98; FX 3 crystal

    // Amplitude begins at 1841.0,
    // fades to 20% at 0.7s, keyoff fades to 20% in 0.7s.
    { 1620,1620,  0,    660,   660 }, // 1524: f21GM104; f21GM99; f41GM104; f41GM99; FX 4 atmosphere; Sitar

    // Amplitude begins at  293.5, peaks  369.0 at 0.0s,
    // fades to 20% at 1.7s, keyoff fades to 20% in 1.7s.
    { 1621,1621,  0,   1713,  1713 }, // 1525: b41M100; f21GM100; f41GM100; FX 5 brightness; SB100.in

    // Amplitude begins at 2290.1,
    // fades to 20% at 40.0s, keyoff fades to 20% in 2.8s.
    { 1622,1622,  0,  40000,  2780 }, // 1526: b41M101; b41M102; f21GM101; f21GM102; f32GM100; f32GM101; f32GM102; f41GM101; f41GM102; FX 5 brightness; FX 6 goblins; FX 7 echoes; belshort

    // Amplitude begins at  421.8, peaks  475.3 at 5.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 1623,1623,  0,  40000,   193 }, // 1527: b41M103; f21GM103; f41GM103; FX 8 sci-fi; SB103.in

    // Amplitude begins at   12.4, peaks  524.5 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1624,1624,  0,  40000,     0 }, // 1528: b41M106; f21GM106; f32GM106; f41GM106; Shamisen; fstrp2.i

    // Amplitude begins at    1.7, peaks 1554.6 at 13.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1625,1625,  0,  40000,    80 }, // 1529: b41M107; b41M108; b41M109; b41M93; f21GM107; f21GM108; f21GM109; f41GM107; f41GM108; f41GM109; f41GM93; Bagpipe; Kalimba; Koto; Pad 6 metallic; flute.in

    // Amplitude begins at    0.0, peaks  761.2 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1626,1626,  0,  40000,     6 }, // 1530: b41M110; b41M111; f21GM110; f21GM111; f41GM110; f41GM111; Fiddle; Shanai; flute2.i

    // Amplitude begins at 1501.8, peaks 3140.0 at 7.8s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.5s.
    { 1627,1627,  0,  40000,   473 }, // 1531: f21GM114; Steel Drums

    // Amplitude begins at 2925.3, peaks 3004.4 at 0.0s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 1628,1628,  0,    626,   626 }, // 1532: f21GM115; Woodblock

    // Amplitude begins at  193.5, peaks  451.4 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 281,281,  0,     86,    86 }, // 1533: b41M119; f21GM119; f32GM119; f41GM119; f53GM117; Melodic Tom; Reverse Cymbal; cymbal.i

    // Amplitude begins at 2089.3, peaks 2568.5 at 0.0s,
    // fades to 20% at 2.3s, keyoff fades to 20% in 2.3s.
    { 1629,1629,  0,   2333,  2333 }, // 1534: b41M120; f21GM120; f41GM120; Guitar FretNoise; entbell3

    // Amplitude begins at  540.2, peaks  548.9 at 0.0s,
    // fades to 20% at 2.4s, keyoff fades to 20% in 2.4s.
    { 1630,1630,  0,   2373,  2373 }, // 1535: b41M121; f21GM121; f41GM121; Breath Noise; triangle

    // Amplitude begins at    0.8, peaks 2934.2 at 0.1s,
    // fades to 20% at 0.7s, keyoff fades to 20% in 0.7s.
    { 1631,1631,  0,    713,   713 }, // 1536: b41M122; f21GM122; f32GM122; f41GM122; Seashore; synbass4

    // Amplitude begins at    7.2, peaks 3638.0 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 1632,1632,  0,  40000,   160 }, // 1537: f21GM124; Telephone

    // Amplitude begins at 2750.5,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1633,1633,  0,     66,    66 }, // 1538: b41P35; f21GP35; f27GP27; f27GP28; f27GP29; f27GP30; f27GP31; f27GP35; f41GP35; Ac Bass Drum; bdc1.ins

    // Amplitude begins at   15.7, peaks  816.0 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1634,1634, 38,    120,   120 }, // 1539: f21GP36; f21GP70; Bass Drum 1; Maracas

    // Amplitude begins at  742.9,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1635,1635,  0,     80,    80 }, // 1540: f21GP37; f41GP37; Side Stick

    // Amplitude begins at  635.2,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1636,1636, 17,      6,     6 }, // 1541: f21GP38; f21GP39; f21GP40; Acoustic Snare; Electric Snare; Hand Clap

    // Amplitude begins at  404.3,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1637,1637, 30,     80,    80 }, // 1542: f21GP42; f27GP51; f27GP53; f27GP54; f27GP59; f41GP42; Closed High Hat; Ride Bell; Ride Cymbal 1; Ride Cymbal 2; Tambourine

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1638,1638,164,      0,     0 }, // 1543: f21GP44; f21GP47; f21GP69; f21GP71; f27GP55; f27GP71; f32GP44; f32GP46; f32GP51; f32GP52; f32GP54; f32GP69; f32GP70; f32GP71; f32GP72; f32GP73; f32GP75; f32GP80; f32GP81; f32GP82; f32GP85; f41GP44; f41GP47; f41GP69; f41GP70; f41GP71; Cabasa; Castanets; Chinese Cymbal; Claves; Long Whistle; Low-Mid Tom; Maracas; Mute Triangle; Open High Hat; Open Triangle; Pedal High Hat; Ride Cymbal 1; Shaker; Short Guiro; Short Whistle; Splash Cymbal; Tambourine

    // Amplitude begins at  257.7, peaks  260.3 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1639,1639, 46,     86,    86 }, // 1544: f21GP46; f41GP46; Open High Hat

    // Amplitude begins at  569.0,
    // fades to 20% at 3.3s, keyoff fades to 20% in 3.3s.
    { 1630,1630, 41,   3313,  3313 }, // 1545: f21GP63; f27GP80; f27GP81; f27GP83; f27GP84; f41GP63; Bell Tree; Jingle Bell; Mute Triangle; Open High Conga; Open Triangle

    // Amplitude begins at 2330.8,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 1640,1640, 18,    186,   186 }, // 1546: f21GP72; f21GP73; f21GP75; f27GP41; f27GP43; f27GP45; f27GP47; f27GP48; f27GP50; f27GP60; f27GP61; f27GP62; f27GP63; f27GP64; f27GP65; f27GP66; f27GP72; f27GP73; f27GP74; f27GP86; f27GP87; f41GP72; f41GP73; f41GP75; Claves; High Bongo; High Floor Tom; High Timbale; High Tom; High-Mid Tom; Long Guiro; Long Whistle; Low Bongo; Low Conga; Low Floor Tom; Low Timbale; Low Tom; Low-Mid Tom; Mute High Conga; Mute Surdu; Open High Conga; Open Surdu; Short Guiro

    // Amplitude begins at  461.5, peaks 1649.3 at 0.1s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.0s.
    { 1641,1641,  0,    146,    13 }, // 1547: f23GM0; f23GM125; AcouGrandPiano; Helicopter

    // Amplitude begins at  840.5, peaks 3900.2 at 0.1s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.0s.
    { 1642,1642,  0,    346,     6 }, // 1548: f23GM24; Acoustic Guitar1

    // Amplitude begins at  257.8, peaks  876.2 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1643,1643,  0,  40000,     0 }, // 1549: f23GM25; f23GM64; Acoustic Guitar2; Soprano Sax

    // Amplitude begins at  306.6, peaks 1034.4 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1644,1644,  0,  40000,    20 }, // 1550: f23GM26; f23GM27; f23GM68; Electric Guitar1; Electric Guitar2; Oboe

    // Amplitude begins at 1847.8, peaks 3103.6 at 0.0s,
    // fades to 20% at 0.5s, keyoff fades to 20% in 0.0s.
    { 1645,1645,  0,    486,     6 }, // 1551: f23GM30; Distorton Guitar

    // Amplitude begins at    8.9, peaks 1658.4 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1646,1646,  0,  40000,    53 }, // 1552: f23GM48; String Ensemble1

    // Amplitude begins at 2305.2, peaks 2704.5 at 32.7s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1647,1647,  0,  40000,     0 }, // 1553: f23GM50; Synth Strings 1

    // Amplitude begins at  122.0, peaks 2773.4 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 1648,1648,  0,    280,   280 }, // 1554: f23GM51; SynthStrings 2

    // Amplitude begins at  519.7, peaks 1404.2 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1649,1649,  0,  40000,    26 }, // 1555: f23GM65; Alto Sax

    // Amplitude begins at  759.2, peaks  842.2 at 0.0s,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 1650,1650,  0,    406,   406 }, // 1556: f23GM122; f23GM66; Seashore; Tenor Sax

    // Amplitude begins at    6.1, peaks 2905.4 at 0.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1651,1651,  0,  40000,     6 }, // 1557: f23GM71; Clarinet

    // Amplitude begins at  825.3,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 1652,1652,  0,    233,   233 }, // 1558: f23GM72; Piccolo

    // Amplitude begins at  733.0, peaks  927.8 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1653,1653,  0,  40000,     0 }, // 1559: f23GM76; Bottle Blow

    // Amplitude begins at  306.6, peaks 1195.7 at 1.5s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 1654,1654,  0,  40000,   213 }, // 1560: f23GM77; Shakuhachi

    // Amplitude begins at  871.7, peaks 1102.6 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1655,1655,  0,  40000,   146 }, // 1561: f23GM80; Lead 1 squareea

    // Amplitude begins at  658.2, peaks  868.7 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 1656,1656,  0,    300,   300 }, // 1562: f23GM81; Lead 2 sawtooth

    // Amplitude begins at    2.6, peaks 1130.3 at 1.4s,
    // fades to 20% at 1.5s, keyoff fades to 20% in 0.1s.
    { 1657,1657,  0,   1460,   106 }, // 1563: f23GM86; Lead 7 fifths

    // Amplitude begins at  305.2, peaks 1032.6 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1658,1658,  0,  40000,     0 }, // 1564: f23GM88; Pad 1 new age

    // Amplitude begins at  530.5, peaks  926.4 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1659,1659,  0,  40000,     0 }, // 1565: f23GM91; Pad 4 choir

    // Amplitude begins at  632.6, peaks 1164.8 at 25.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1660,1660,  0,  40000,     6 }, // 1566: f23GM92; f23GM93; Pad 5 bowedpad; Pad 6 metallic

    // Amplitude begins at  858.2, peaks  908.1 at 0.0s,
    // fades to 20% at 1.0s, keyoff fades to 20% in 1.0s.
    { 1661,1661,  0,    960,   960 }, // 1567: f23GM94; Pad 7 halo

    // Amplitude begins at   36.7, peaks 2595.8 at 0.1s,
    // fades to 20% at 2.6s, keyoff fades to 20% in 2.6s.
    { 1662,1662,  0,   2640,  2640 }, // 1568: f23GM105; f23GM95; Banjo; Pad 8 sweep

    // Amplitude begins at 2646.0, peaks 3204.4 at 0.0s,
    // fades to 20% at 1.8s, keyoff fades to 20% in 1.8s.
    { 1663,1663,  0,   1800,  1800 }, // 1569: f23GM97; FX 2 soundtrack

    // Amplitude begins at 2489.2, peaks 3085.3 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 1664,1664,  0,    240,   240 }, // 1570: f23GM104; f23GM98; FX 3 crystal; Sitar

    // Amplitude begins at 1231.2, peaks 1904.2 at 39.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1665,1665,  0,  40000,   113 }, // 1571: f23GM99; FX 4 atmosphere

    // Amplitude begins at    0.5, peaks 2936.3 at 0.3s,
    // fades to 20% at 2.3s, keyoff fades to 20% in 2.3s.
    { 1666,1666,  0,   2273,  2273 }, // 1572: f23GM100; FX 5 brightness

    // Amplitude begins at 2677.3,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1667,1667,  0,    146,   146 }, // 1573: f23GM103; FX 8 sci-fi

    // Amplitude begins at  131.8, peaks 3435.8 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1668,1668,  0,  40000,   106 }, // 1574: f23GM107; f23GM111; Koto; Shanai

    // Amplitude begins at 2468.3, peaks 3081.0 at 0.0s,
    // fades to 20% at 0.5s, keyoff fades to 20% in 0.5s.
    { 1669,1669,  0,    486,   486 }, // 1575: f23GM112; Tinkle Bell

    // Amplitude begins at  117.8, peaks 2045.4 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1670,1670,  0,    126,   126 }, // 1576: f23GM116; Taiko Drum

    // Amplitude begins at  786.2,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 1671,1671,  0,    233,   233 }, // 1577: f23GM117; Melodic Tom

    // Amplitude begins at 1475.8,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1672,1672,  0,  40000,    53 }, // 1578: f23GM119; Reverse Cymbal

    // Amplitude begins at 1401.4,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1673,1673, 16,     26,    26 }, // 1579: f23GP36; Bass Drum 1

    // Amplitude begins at  652.9,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1674,1674, 30,     46,    46 }, // 1580: f23GP37; Side Stick

    // Amplitude begins at  634.8,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1675,1675, 32,     20,    20 }, // 1581: f23GP38; f23GP40; f23GP53; f23GP55; f23GP67; Acoustic Snare; Electric Snare; High Agogo; Ride Bell; Splash Cymbal

    // Amplitude begins at  733.2,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1676,1676,  2,      6,     6 }, // 1582: f23GP39; Hand Clap

    // Amplitude begins at  589.6,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1677,1677, 62,     20,    20 }, // 1583: f23GP42; Closed High Hat

    // Amplitude begins at  345.7,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1678,1678,110,    120,   120 }, // 1584: f23GP49; Crash Cymbal 1

    // Amplitude begins at 1370.3, peaks 1517.9 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 1679,1679,110,    226,   226 }, // 1585: f23GP51; Ride Cymbal 1

    // Amplitude begins at 1550.1, peaks 2197.7 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.4s.
    { 1680,1681,  0,  40000,   413 }, // 1586: f24GM48; String Ensemble1

    // Amplitude begins at  720.5, peaks 2100.9 at 0.1s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.0s.
    { 1682,1682,  0,    166,    13 }, // 1587: f24GM65; Alto Sax

    // Amplitude begins at  996.0, peaks 1918.0 at 39.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1683,1684,  0,  40000,   126 }, // 1588: f24GM74; Recorder

    // Amplitude begins at  334.2, peaks 1617.5 at 0.1s,
    // fades to 20% at 33.9s, keyoff fades to 20% in 0.0s.
    { 1685,1686,  0,  33866,     6 }, // 1589: f24GM88; Pad 1 new age

    // Amplitude begins at  773.5, peaks  944.3 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1687,1688,  0,  40000,     6 }, // 1590: f24GM91; Pad 4 choir

    // Amplitude begins at 3265.7, peaks 3579.9 at 0.0s,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.9s.
    { 1689,1689,  0,    886,   886 }, // 1591: f25GM1; BrightAcouGrand

    // Amplitude begins at 1032.0, peaks 3384.0 at 35.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 1690,1690,  0,  40000,   213 }, // 1592: f25GM12; f25GM13; f25GM14; Marimba; Tubular Bells; Xylophone

    // Amplitude begins at  704.7, peaks  918.1 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.4s.
    { 1691,1691,  0,  40000,   353 }, // 1593: f25GM33; Electric Bass 1

    // Amplitude begins at    0.4, peaks 1602.4 at 0.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 1692,1692,  0,  40000,   180 }, // 1594: f25GM34; Electric Bass 2

    // Amplitude begins at 1416.2,
    // fades to 20% at 2.2s, keyoff fades to 20% in 2.2s.
    { 1693,1693,  0,   2233,  2233 }, // 1595: f25GM103; f25GM38; FX 8 sci-fi; Synth Bass 1

    // Amplitude begins at  163.4, peaks  369.1 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1694,1694,  0,  40000,    93 }, // 1596: f25GM48; String Ensemble1

    // Amplitude begins at  902.7, peaks  928.8 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1695,1695,  0,  40000,   106 }, // 1597: f25GM49; String Ensemble2

    // Amplitude begins at 2006.2,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 1696,1696,  0,    226,   226 }, // 1598: f25GM58; Tuba

    // Amplitude begins at 1359.8,
    // fades to 20% at 0.8s, keyoff fades to 20% in 0.8s.
    { 1697,1697,  0,    820,   820 }, // 1599: f25GM59; f25GM60; French Horn; Muted Trumpet

    // Amplitude begins at    3.5, peaks 1781.1 at 0.0s,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    { 1698,1698,  0,   1160,  1160 }, // 1600: f25GM70; f25GM71; Bassoon; Clarinet

    // Amplitude begins at  901.6, peaks 3762.5 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1699,1699,  0,  40000,   106 }, // 1601: f25GM72; f25GM74; Piccolo; Recorder

    // Amplitude begins at 1440.7, peaks 2051.6 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1700,1700,  0,  40000,    66 }, // 1602: f25GM73; Flute

    // Amplitude begins at  922.6, peaks 1046.1 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1701,1701,  0,  40000,    13 }, // 1603: f25GM82; f25GM83; Lead 3 calliope; Lead 4 chiff

    // Amplitude begins at  733.3, peaks 3169.8 at 0.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1702,1702,  0,  40000,    66 }, // 1604: f25GM89; Pad 2 warm

    // Amplitude begins at 3197.2, peaks 3337.5 at 0.0s,
    // fades to 20% at 2.2s, keyoff fades to 20% in 2.2s.
    { 1703,1703,  0,   2220,  2220 }, // 1605: f25GM102; FX 7 echoes

    // Amplitude begins at 1491.5, peaks 1508.5 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 1704,1704,  0,    293,   293 }, // 1606: f25GM104; Sitar

    // Amplitude begins at 4381.8, peaks 5538.8 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.3s.
    { 1705,1705,  0,  40000,   300 }, // 1607: f25GM105; Banjo

    // Amplitude begins at 4157.4, peaks 5643.2 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 1706,1706,  0,  40000,   160 }, // 1608: f25GM106; Shamisen

    // Amplitude begins at 1789.5, peaks 4732.6 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 1707,1707,  0,  40000,   193 }, // 1609: f25GM107; Koto

    // Amplitude begins at 2505.8,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1708,1708,  0,     40,    40 }, // 1610: f25GM110; Fiddle

    // Amplitude begins at 1215.0, peaks 1563.1 at 0.0s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 1709,1709,  0,    580,   580 }, // 1611: f25GM111; Shanai

    // Amplitude begins at  805.0,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 270,270,  0,     66,    66 }, // 1612: f25GM113; Agogo Bells

    // Amplitude begins at 1244.9, peaks 1355.7 at 20.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1710,1710,  0,  40000,     0 }, // 1613: f25GM114; Steel Drums

    // Amplitude begins at 1711.8,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1711,1711,  0,     26,    26 }, // 1614: f25GM117; f47GM116; Melodic Tom; Taiko Drum

    // Amplitude begins at 2097.0,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 1712,1712,  0,    573,   573 }, // 1615: f25GM120; Guitar FretNoise

    // Amplitude begins at  978.8, peaks 2438.0 at 1.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 2.4s.
    { 1713,1713,  0,  40000,  2373 }, // 1616: f25GM123; Bird Tweet

    // Amplitude begins at  773.0,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 1714,1714,  0,    153,   153 }, // 1617: f25GM124; Telephone

    // Amplitude begins at  382.3,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 280,280,  0,    580,   580 }, // 1618: f25GM125; Helicopter

    // Amplitude begins at  751.2,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1715,1715,  0,     80,    80 }, // 1619: f25GM126; Applause/Noise

    // Amplitude begins at 1244.9, peaks 1361.7 at 38.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1710,1710,  9,  40000,     0 }, // 1620: f25GP38; f25GP39; f25GP40; Acoustic Snare; Electric Snare; Hand Clap

    // Amplitude begins at  372.5,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 280,280, 46,    566,   566 }, // 1621: f25GP49; Crash Cymbal 1

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1715,1715,142,      0,     0 }, // 1622: f25GP54; Tambourine

    // Amplitude begins at   72.0, peaks 1346.1 at 3.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1716,1716,  0,  40000,     0 }, // 1623: f26GM24; Acoustic Guitar1

    // Amplitude begins at 2918.7, peaks 3474.0 at 0.3s,
    // fades to 20% at 3.0s, keyoff fades to 20% in 3.0s.
    { 1717,1717,  0,   3020,  3020 }, // 1624: f27GM0; f27GM1; f27GM2; f27GM3; f27GM4; f27GM5; f27GM84; AcouGrandPiano; BrightAcouGrand; Chorused Piano; ElecGrandPiano; Honky-tonkPiano; Lead 5 charang; Rhodes Piano

    // Amplitude begins at 1649.5, peaks 1722.9 at 0.1s,
    // fades to 20% at 2.2s, keyoff fades to 20% in 2.2s.
    { 1718,1718,  0,   2213,  2213 }, // 1625: f27GM7; Clavinet

    // Amplitude begins at 1957.6, peaks 2432.2 at 0.0s,
    // fades to 20% at 0.7s, keyoff fades to 20% in 0.7s.
    { 1719,1719,  0,    680,   680 }, // 1626: f27GM112; f27GM8; Celesta; Tinkle Bell

    // Amplitude begins at 2416.6, peaks 2588.7 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 1720,1720,  0,    340,   340 }, // 1627: f27GM9; Glockenspiel

    // Amplitude begins at 1506.4, peaks 1701.2 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 1721,1721,  0,    293,   293 }, // 1628: f27GM10; Music box

    // Amplitude begins at 1373.8, peaks 1520.5 at 1.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.1s.
    { 1722,1722,  0,  40000,  1086 }, // 1629: f27GM11; Vibraphone

    // Amplitude begins at 1200.3, peaks 1334.3 at 35.8s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 1723,1723,  0,  40000,   246 }, // 1630: f27GM12; Marimba

    // Amplitude begins at 2344.5, peaks 2672.0 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1724,1724,  0,    120,   120 }, // 1631: f27GM13; Xylophone

    // Amplitude begins at 2876.8, peaks 3174.3 at 36.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.9s.
    { 1725,1725,  0,  40000,   913 }, // 1632: f27GM14; f27GM98; FX 3 crystal; Tubular Bells

    // Amplitude begins at 2889.8, peaks 3174.3 at 36.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.9s.
    { 1726,1726,  0,  40000,   913 }, // 1633: f27GM15; Dulcimer

    // Amplitude begins at 2276.4, peaks 2841.5 at 10.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 1727,1727,  0,  40000,   213 }, // 1634: f27GM16; f27GM17; f27GM18; f27GM19; Church Organ; Hammond Organ; Percussive Organ; Rock Organ

    // Amplitude begins at    0.0, peaks  907.6 at 26.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.3s.
    { 1728,1728,  0,  40000,   280 }, // 1635: f27GM20; Reed Organ

    // Amplitude begins at 2056.3, peaks 2581.7 at 13.8s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1729,1729,  0,  40000,     6 }, // 1636: f27GM21; f27GM23; Accordion; Tango Accordion

    // Amplitude begins at  892.3, peaks 2695.9 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1730,1730,  0,  40000,   140 }, // 1637: f27GM22; Harmonica

    // Amplitude begins at 1214.5,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 1731,1731,  0,    300,   300 }, // 1638: f27GM24; f27GM26; f27GM27; f27GM28; Acoustic Guitar1; Electric Guitar1; Electric Guitar2; Electric Guitar3

    // Amplitude begins at 1214.5,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 1732,1732,  0,    300,   300 }, // 1639: f27GM25; Acoustic Guitar2

    // Amplitude begins at  917.5, peaks 1055.6 at 1.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1733,1733,  0,  40000,     0 }, // 1640: f27GM29; Overdrive Guitar

    // Amplitude begins at    0.3, peaks 1274.3 at 0.1s,
    // fades to 20% at 0.7s, keyoff fades to 20% in 0.7s.
    { 1734,1734,  0,    660,   660 }, // 1641: f27GM31; Guitar Harmonics

    // Amplitude begins at 1183.8,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 1735,1735,  0,    260,   260 }, // 1642: f27GM32; f27GM33; f27GM34; f27GM35; f27GM36; f27GM37; f27GM38; f27GM39; f27GM43; Acoustic Bass; Contrabass; Electric Bass 1; Electric Bass 2; Fretless Bass; Slap Bass 1; Slap Bass 2; Synth Bass 1; Synth Bass 2

    // Amplitude begins at    0.2, peaks 2958.7 at 0.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.3s.
    { 1736,1736,  0,  40000,  1253 }, // 1643: f27GM40; f27GM41; f27GM42; f27GM44; f27GM48; f27GM49; Cello; String Ensemble1; String Ensemble2; Tremulo Strings; Viola; Violin

    // Amplitude begins at  789.6,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1737,1737,  0,     80,    80 }, // 1644: f27GM45; Pizzicato String

    // Amplitude begins at 3218.7, peaks 4258.0 at 0.0s,
    // fades to 20% at 1.4s, keyoff fades to 20% in 1.4s.
    { 1738,1738,  0,   1380,  1380 }, // 1645: f27GM46; Orchestral Harp

    // Amplitude begins at 2994.0, peaks 3348.4 at 0.1s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.1s.
    { 1739,1739,  0,    300,    80 }, // 1646: f27GM47; Timpany

    // Amplitude begins at    0.3, peaks 2386.5 at 0.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.6s.
    { 1740,1740,  0,  40000,  1586 }, // 1647: f27GM50; f27GM51; Synth Strings 1; SynthStrings 2

    // Amplitude begins at    0.0, peaks  871.0 at 0.4s,
    // fades to 20% at 2.2s, keyoff fades to 20% in 2.2s.
    { 1741,1741,  0,   2240,  2240 }, // 1648: f27GM52; f27GM53; f27GM54; f27GM85; Choir Aahs; Lead 6 voice; Synth Voice; Voice Oohs

    // Amplitude begins at 1391.9, peaks 3935.3 at 0.1s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.0s.
    { 1742,1742,  0,    146,    13 }, // 1649: f27GM55; Orchestra Hit

    // Amplitude begins at  466.8, peaks  555.0 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1743,1743,  0,  40000,     0 }, // 1650: f27GM56; f27GM59; f27GM61; f27GM62; f27GM63; f27GM64; f27GM65; f27GM66; f27GM67; Alto Sax; Baritone Sax; Brass Section; Muted Trumpet; Soprano Sax; Synth Brass 1; Synth Brass 2; Tenor Sax; Trumpet

    // Amplitude begins at    0.3, peaks 1306.4 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1744,1744,  0,  40000,     0 }, // 1651: f27GM57; Trombone

    // Amplitude begins at   42.9, peaks 3678.6 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 1745,1745,  0,  40000,   160 }, // 1652: f27GM58; Tuba

    // Amplitude begins at    0.0, peaks  873.1 at 0.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.7s.
    { 1746,1746,  0,  40000,   673 }, // 1653: b41M52; b41M54; f27GM52; f32GM52; f32GM54; f41GM52; f41GM54; Choir Aahs; Synth Voice; violin.i

    // Amplitude begins at   43.0, peaks 3112.7 at 0.1s,
    // fades to 20% at 1.5s, keyoff fades to 20% in 0.0s.
    { 1747,1747,  0,   1453,     6 }, // 1654: f27GM60; f27GM69; English Horn; French Horn

    // Amplitude begins at 1176.3, peaks 4854.4 at 5.0s,
    // fades to 20% at 5.0s, keyoff fades to 20% in 5.0s.
    { 1748,1748,  0,   5026,  5026 }, // 1655: f27GM70; Bassoon

    // Amplitude begins at  781.8, peaks 4405.6 at 28.5s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1749,1749,  0,  40000,     0 }, // 1656: f27GM71; Clarinet

    // Amplitude begins at    4.4, peaks 1702.8 at 19.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1750,1750,  0,  40000,    73 }, // 1657: f27GM72; f27GM73; f27GM74; f27GM76; f27GM77; Bottle Blow; Flute; Piccolo; Recorder; Shakuhachi

    // Amplitude begins at    3.7, peaks 1471.4 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1751,1751,  0,    113,   113 }, // 1658: f27GM75; Pan Flute

    // Amplitude begins at   31.1, peaks  802.5 at 39.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1752,1752,  0,  40000,    80 }, // 1659: f27GM78; f27GM79; Ocarina; Whistle

    // Amplitude begins at 1586.0,
    // fades to 20% at 0.7s, keyoff fades to 20% in 0.7s.
    { 1753,1753,  0,    726,   726 }, // 1660: f27GM80; Lead 1 squareea

    // Amplitude begins at    6.4, peaks 2633.1 at 14.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1754,1754,  0,  40000,    40 }, // 1661: f27GM121; f27GM82; f27GM83; Breath Noise; Lead 3 calliope; Lead 4 chiff

    // Amplitude begins at    0.0, peaks 1801.6 at 2.3s,
    // fades to 20% at 4.4s, keyoff fades to 20% in 4.4s.
    { 1755,1755,  0,   4366,  4366 }, // 1662: f27GM86; f27GM95; Lead 7 fifths; Pad 8 sweep

    // Amplitude begins at  194.5, peaks  528.4 at 12.5s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 1756,1756,  0,  40000,   206 }, // 1663: f27GM87; Lead 8 brass

    // Amplitude begins at 1101.9, peaks 1330.0 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.3s.
    { 1757,1757,  0,  40000,   346 }, // 1664: f27GM88; Pad 1 new age

    // Amplitude begins at    0.0, peaks 4215.7 at 1.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.5s.
    { 1758,1758,  0,  40000,   460 }, // 1665: f27GM89; Pad 2 warm

    // Amplitude begins at    0.6, peaks 1883.9 at 0.1s,
    // fades to 20% at 8.3s, keyoff fades to 20% in 0.1s.
    { 1759,1759,  0,   8286,    80 }, // 1666: f27GM90; Pad 3 polysynth

    // Amplitude begins at  102.6, peaks 2676.3 at 18.5s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.1s.
    { 1760,1760,  0,  40000,  1053 }, // 1667: f27GM91; Pad 4 choir

    // Amplitude begins at    0.0, peaks 2949.2 at 10.7s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.8s.
    { 1761,1761,  0,  40000,   806 }, // 1668: f27GM92; Pad 5 bowedpad

    // Amplitude begins at    0.0, peaks 2219.9 at 20.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.9s.
    { 1762,1762,  0,  40000,   926 }, // 1669: f27GM93; Pad 6 metallic

    // Amplitude begins at    0.0, peaks 1373.2 at 0.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.9s.
    { 1763,1763,  0,  40000,   946 }, // 1670: f27GM94; f27GM96; FX 1 rain; Pad 7 halo

    // Amplitude begins at    0.0, peaks 2561.6 at 0.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.0s.
    { 1764,1764,  0,  40000,  1033 }, // 1671: f27GM97; FX 2 soundtrack

    // Amplitude begins at 2867.5, peaks 3174.3 at 36.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.8s.
    { 1765,1765,  0,  40000,  1840 }, // 1672: f27GM99; FX 4 atmosphere

    // Amplitude begins at 2521.6, peaks 3180.3 at 3.7s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.7s.
    { 1766,1766,  0,  40000,  1700 }, // 1673: f27GM100; FX 5 brightness

    // Amplitude begins at    0.0, peaks 3803.8 at 2.2s,
    // fades to 20% at 6.4s, keyoff fades to 20% in 0.1s.
    { 1767,1767,  0,   6433,    60 }, // 1674: f27GM101; FX 6 goblins

    // Amplitude begins at    0.0, peaks 3324.2 at 0.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.8s.
    { 1768,1768,  0,  40000,   820 }, // 1675: f27GM102; FX 7 echoes

    // Amplitude begins at 1702.0, peaks 2452.8 at 0.0s,
    // fades to 20% at 2.4s, keyoff fades to 20% in 2.4s.
    { 1769,1769,  0,   2386,  2386 }, // 1676: f27GM103; FX 8 sci-fi

    // Amplitude begins at 2881.0,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 1770,1770,  0,    580,   580 }, // 1677: f27GM104; Sitar

    // Amplitude begins at 1353.1, peaks 2445.7 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.1s.
    { 1771,1771,  0,  40000,  1140 }, // 1678: f27GM105; Banjo

    // Amplitude begins at  119.6, peaks 2327.3 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 1772,1772,  0,    173,   173 }, // 1679: f27GM106; Shamisen

    // Amplitude begins at  791.1,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1773,1773,  0,     80,    80 }, // 1680: f27GM107; Koto

    // Amplitude begins at 2520.9,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1774,1774,  0,     66,    66 }, // 1681: f27GM108; Kalimba

    // Amplitude begins at 1224.1, peaks 1285.0 at 29.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1775,1775,  0,  40000,    20 }, // 1682: f27GM109; Bagpipe

    // Amplitude begins at 1224.1, peaks 1250.4 at 24.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1776,1776,  0,  40000,    13 }, // 1683: f27GM110; f27GM111; Fiddle; Shanai

    // Amplitude begins at 2746.0,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.9s.
    { 1777,1777,  0,  40000,  1913 }, // 1684: f27GM113; Agogo Bells

    // Amplitude begins at 3522.3,
    // fades to 20% at 40.0s, keyoff fades to 20% in 3.6s.
    { 1778,1778,  0,  40000,  3593 }, // 1685: f27GM114; Steel Drums

    // Amplitude begins at 1197.1,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1779,1779,  0,     40,    40 }, // 1686: f27GM115; Woodblock

    // Amplitude begins at 2504.8,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1780,1780,  0,    106,   106 }, // 1687: f27GM116; f27GM117; f27GM118; Melodic Tom; Synth Drum; Taiko Drum

    // Amplitude begins at    0.0, peaks  868.5 at 2.3s,
    // fades to 20% at 2.4s, keyoff fades to 20% in 2.4s.
    { 1781,1781,  0,   2366,  2366 }, // 1688: f27GM119; Reverse Cymbal

    // Amplitude begins at    0.0, peaks  789.0 at 0.3s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 1782,1782,  0,    313,   313 }, // 1689: f27GM120; Guitar FretNoise

    // Amplitude begins at    0.0, peaks  400.7 at 24.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 2.3s.
    { 1783,1783,  0,  40000,  2306 }, // 1690: f27GM122; Seashore

    // Amplitude begins at  552.5, peaks 1446.0 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1784,1784,  0,  40000,   146 }, // 1691: f27GM123; Bird Tweet

    // Amplitude begins at  962.5, peaks 1638.4 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 2.1s.
    { 1785,1785,  0,  40000,  2146 }, // 1692: f27GM124; Telephone

    // Amplitude begins at 1770.3, peaks 2028.8 at 20.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 4.0s.
    { 1786,1786,  0,  40000,  4020 }, // 1693: f27GM125; Helicopter

    // Amplitude begins at    0.0, peaks  876.6 at 1.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 1787,1787,  0,  40000,   153 }, // 1694: f27GM126; Applause/Noise

    // Amplitude begins at  218.7,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1788,1788, 28,      6,     6 }, // 1695: f27GP42; Closed High Hat

    // Amplitude begins at  368.3, peaks  379.8 at 0.0s,
    // fades to 20% at 1.6s, keyoff fades to 20% in 1.6s.
    { 1789,1789, 14,   1566,  1566 }, // 1696: f27GP44; f27GP46; Open High Hat; Pedal High Hat

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1790,1790,142,      0,     0 }, // 1697: f27GP49; f27GP52; f27GP57; Chinese Cymbal; Crash Cymbal 1; Crash Cymbal 2

    // Amplitude begins at 2532.3,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.8s.
    { 1791,1791,100,  40000,   846 }, // 1698: f27GP56; Cow Bell

    // Amplitude begins at 1020.3,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1792,1792,  0,     86,    86 }, // 1699: f27GP58; Vibraslap

    // Amplitude begins at    0.0, peaks  434.1 at 0.1s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1793,1793, 12,     93,    93 }, // 1700: f27GP69; f27GP70; f27GP82; Cabasa; Maracas; Shaker

    // Amplitude begins at 1302.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1779,1779, 19,     33,    33 }, // 1701: f27GP76; f27GP77; f27GP78; f27GP79; High Wood Block; Low Wood Block; Mute Cuica; Open Cuica

    // Amplitude begins at 1719.5, peaks 1740.6 at 0.0s,
    // fades to 20% at 2.2s, keyoff fades to 20% in 2.2s.
    { 1794,1794,  0,   2240,  2240 }, // 1702: f29GM7; f30GM7; Clavinet

    // Amplitude begins at 1451.4, peaks 1492.7 at 6.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1795,1795,  0,  40000,    20 }, // 1703: f29GM14; Tubular Bells

    // Amplitude begins at 2805.0, peaks 2835.6 at 0.0s,
    // fades to 20% at 1.0s, keyoff fades to 20% in 1.0s.
    { 1796,1796,  0,   1046,  1046 }, // 1704: f29GM22; f29GM23; f30GM22; f30GM23; Harmonica; Tango Accordion

    // Amplitude begins at    7.2, peaks 2824.8 at 0.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1797,1797,  0,  40000,    26 }, // 1705: f29GM24; f29GM25; f29GM27; f30GM24; f30GM25; f30GM27; Acoustic Guitar1; Acoustic Guitar2; Electric Guitar2

    // Amplitude begins at    0.0, peaks 2031.4 at 1.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1798,1798,  0,  40000,   106 }, // 1706: f29GM33; Electric Bass 1

    // Amplitude begins at 2857.8, peaks 2984.9 at 0.0s,
    // fades to 20% at 1.9s, keyoff fades to 20% in 1.9s.
    { 1799,1799,  0,   1886,  1886 }, // 1707: f29GM40; f29GM98; f30GM40; f30GM98; FX 3 crystal; Violin

    // Amplitude begins at  858.7, peaks 1124.2 at 14.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.5s.
    { 1800,1800,  0,  40000,   480 }, // 1708: f29GM50; Synth Strings 1

    // Amplitude begins at 1381.5,
    // fades to 20% at 0.8s, keyoff fades to 20% in 0.8s.
    { 1801,1801,  0,    793,   793 }, // 1709: f29GM59; Muted Trumpet

    // Amplitude begins at  835.6,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.9s.
    { 1802,1802,  0,    886,   886 }, // 1710: f29GM61; Brass Section

    // Amplitude begins at  121.5, peaks 1563.4 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1803,1803,  0,  40000,     6 }, // 1711: f29GM76; Bottle Blow

    // Amplitude begins at 2060.7, peaks 2301.9 at 0.0s,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    { 1804,1804,  0,   1200,  1200 }, // 1712: f29GM102; f30GM102; FX 7 echoes

    // Amplitude begins at  715.4,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1805,1805,  0,     80,    80 }, // 1713: f29GM112; Tinkle Bell

    // Amplitude begins at  879.3,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1806,1806,  0,     20,    20 }, // 1714: f29GM117; Melodic Tom

    // Amplitude begins at  476.1, peaks  487.6 at 0.0s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 136,136,  0,    573,   573 }, // 1715: f29GM119; f29GM125; f29GM127; f30GM119; f30GM125; f30GM127; Gunshot; Helicopter; Reverse Cymbal

    // Amplitude begins at 1272.9,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 168,168,  0,     40,    40 }, // 1716: f29GM120; f30GM120; Guitar FretNoise

    // Amplitude begins at 1191.4,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    { 164,164,  0,   1233,  1233 }, // 1717: f29GM121; f30GM121; Breath Noise

    // Amplitude begins at 1192.9,
    // fades to 20% at 0.8s, keyoff fades to 20% in 0.8s.
    { 167,167,  0,    786,   786 }, // 1718: f29GM126; f30GM126; Applause/Noise

    // Amplitude begins at  947.5,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1807,1807,104,     26,    26 }, // 1719: f29GP54; Tambourine

    // Amplitude begins at 1395.5,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1808,1808, 35,    100,   100 }, // 1720: f29GP66; Low Timbale

    // Amplitude begins at 2530.1,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    { 1809,607,  0,   1166,  1166 }, // 1721: f31GM4; Rhodes Piano

    // Amplitude begins at 4513.1, peaks 5498.9 at 0.0s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 1810,1811,  0,    553,   553 }, // 1722: f31GM8; Celesta

    // Amplitude begins at 1955.1, peaks 2027.5 at 0.0s,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    { 1812,1813,  0,   1153,  1153 }, // 1723: f31GM11; Vibraphone

    // Amplitude begins at 2037.4,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.9s.
    { 1814,1815,  0,    940,   940 }, // 1724: f31GM14; Tubular Bells

    // Amplitude begins at  124.0, peaks 4443.0 at 0.0s,
    // fades to 20% at 1.4s, keyoff fades to 20% in 1.4s.
    { 1816,1817,  0,   1413,  1413 }, // 1725: f31GM46; Orchestral Harp

    // Amplitude begins at 4443.0, peaks 8341.0 at 0.1s,
    // fades to 20% at 1.1s, keyoff fades to 20% in 1.1s.
    { 1818,1819,  0,   1086,  1086 }, // 1726: f31GM47; Timpany

    // Amplitude begins at   42.4, peaks 2407.7 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 1820,1821,  0,  40000,   200 }, // 1727: f31GM48; f31GM49; String Ensemble1; String Ensemble2

    // Amplitude begins at  115.1, peaks 2354.5 at 0.1s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.0s.
    { 1822,1821,  0,    106,    13 }, // 1728: f31GM50; Synth Strings 1

    // Amplitude begins at  874.6, peaks 3792.8 at 2.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.4s.
    { 1823,767,  0,  40000,   386 }, // 1729: f31GM60; French Horn

    // Amplitude begins at  377.2, peaks 1857.9 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 1824,1825,  0,  40000,   193 }, // 1730: f31GM61; Brass Section

    // Amplitude begins at   58.9, peaks 1828.1 at 0.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.3s.
    { 1826,1827,  0,  40000,   313 }, // 1731: f31GM68; Oboe

    // Amplitude begins at    0.0, peaks 1280.1 at 32.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.3s.
    { 1828,1829,  0,  40000,   280 }, // 1732: f31GM74; Recorder

    // Amplitude begins at 3167.4, peaks 3413.8 at 0.0s,
    // fades to 20% at 0.8s, keyoff fades to 20% in 0.8s.
    { 1830,1831,  0,    766,   766 }, // 1733: f31GM88; Pad 1 new age

    // Amplitude begins at  616.1,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1832,1833,  0,     73,    73 }, // 1734: f31GP40; Electric Snare

    // Amplitude begins at 2236.3, peaks 2476.8 at 2.8s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 1834,1834,  0,  40000,   153 }, // 1735: f32GM10; f32GM9; Glockenspiel; Music box

    // Amplitude begins at 2444.8, peaks 3174.3 at 36.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1835,1835,  0,  40000,     6 }, // 1736: f32GM11; Vibraphone

    // Amplitude begins at 1885.5, peaks 2569.5 at 31.8s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1836,1836,  0,  40000,   146 }, // 1737: f32GM12; f32GM13; f32GM14; Marimba; Tubular Bells; Xylophone

    // Amplitude begins at 1726.0, peaks 2320.2 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.1s.
    { 1837,1837,  0,  40000,  1060 }, // 1738: f32GM32; Acoustic Bass

    // Amplitude begins at    0.0, peaks 2879.5 at 0.6s,
    // fades to 20% at 1.8s, keyoff fades to 20% in 1.8s.
    { 1838,1838,  0,   1813,  1813 }, // 1739: f32GM34; f41GM34; f54GM80; Electric Bass 2; Lead 1 squareea

    // Amplitude begins at 2432.1, peaks 3093.3 at 0.0s,
    // fades to 20% at 3.8s, keyoff fades to 20% in 3.8s.
    { 1839,1839,  0,   3840,  3840 }, // 1740: b41M35; f32GM35; f41GM35; Fretless Bass; tincan1.

    // Amplitude begins at  113.7, peaks 3431.6 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1840,1840,  0,  40000,     0 }, // 1741: b41M42; f32GM42; f47GM68; Cello; Oboe; oboe1.in

    // Amplitude begins at  431.3, peaks 2833.7 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1841,1841,  0,    140,   140 }, // 1742: b41M46; f32GM46; f41GM46; Orchestral Harp; javaican

    // Amplitude begins at   73.2, peaks 2460.8 at 0.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.0s.
    { 1842,1842,  0,  40000,  1033 }, // 1743: f32GM48; f32GM50; f53GM73; Flute; String Ensemble1; Synth Strings 1

    // Amplitude begins at    0.0, peaks  810.4 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1843,1843,  0,  40000,     6 }, // 1744: f32GM110; f32GM111; f32GM76; f32GM77; f47GM78; Bottle Blow; Fiddle; Shakuhachi; Shanai; Whistle

    // Amplitude begins at    0.0, peaks  873.0 at 0.1s,
    // fades to 20% at 1.1s, keyoff fades to 20% in 1.1s.
    { 1844,1844,  0,   1140,  1140 }, // 1745: f32GM81; Lead 2 sawtooth

    // Amplitude begins at  307.9, peaks 1040.7 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1845,1845,  0,  40000,     0 }, // 1746: f32GM88; f32GM89; f41GM89; Pad 1 new age; Pad 2 warm

    // Amplitude begins at    0.0, peaks 1051.9 at 0.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1846,1846,  0,  40000,     0 }, // 1747: b41M90; f32GM90; f37GM57; f41GM90; Pad 3 polysynth; Trombone; tromb2.i

    // Amplitude begins at  258.9, peaks  934.3 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1847,1847,  0,  40000,     0 }, // 1748: b41M91; f32GM91; f41GM91; Pad 4 choir; tromb1.i

    // Amplitude begins at   40.6, peaks  855.3 at 0.0s,
    // fades to 20% at 6.8s, keyoff fades to 20% in 6.8s.
    { 1848,1848,  0,   6800,  6800 }, // 1749: f32GM96; FX 1 rain

    // Amplitude begins at 3346.2, peaks 3646.3 at 0.0s,
    // fades to 20% at 1.5s, keyoff fades to 20% in 1.5s.
    { 1849,1849,  0,   1466,  1466 }, // 1750: f32GM97; FX 2 soundtrack

    // Amplitude begins at    1.8, peaks 1857.2 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1850,1850,  0,  40000,    46 }, // 1751: f32GM107; f32GM108; f32GM109; f47GM72; Bagpipe; Kalimba; Koto; Piccolo

    // Amplitude begins at 1809.2,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1851,1851,  0,     40,    40 }, // 1752: f32GM120; Guitar FretNoise

    // Amplitude begins at  810.1,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1852,1852,  0,     73,    73 }, // 1753: f32GM127; Gunshot

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1853,1853,164,      0,     0 }, // 1754: f32GP42; Closed High Hat

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1854,1854,164,      0,     0 }, // 1755: f32GP49; f32GP57; f47GP30; Crash Cymbal 1; Crash Cymbal 2

    // Amplitude begins at    4.3, peaks  870.5 at 0.0s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 1855,1855,  0,    613,   613 }, // 1756: f34GM74; Recorder

    // Amplitude begins at  832.1,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1856,1856, 16,     33,    33 }, // 1757: f34GP0

    // Amplitude begins at 1163.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1857,1857, 16,      6,     6 }, // 1758: f34GP2

    // Amplitude begins at  657.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1858,1858,  0,     20,    20 }, // 1759: f34GP3

    // Amplitude begins at 1409.8, peaks 1463.0 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1859,1859, 18,     13,    13 }, // 1760: f34GP4; f34GP5

    // Amplitude begins at 1372.3,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1860,1860, 20,      6,     6 }, // 1761: f34GP10; f34GP6

    // Amplitude begins at  611.7,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1861,1861,  0,      6,     6 }, // 1762: f34GP7; f34GP8

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1862,1862,209,      0,     0 }, // 1763: f34GP9

    // Amplitude begins at 1762.4, peaks 1799.1 at 0.0s,
    // fades to 20% at 1.1s, keyoff fades to 20% in 1.1s.
    { 1863,1863, 66,   1120,  1120 }, // 1764: f34GP11

    // Amplitude begins at 1762.4, peaks 1799.1 at 0.0s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 1864,1864, 66,    586,   586 }, // 1765: f34GP12

    // Amplitude begins at  486.8,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1865,1865, 16,     20,    20 }, // 1766: f34GP13; f34GP15

    // Amplitude begins at 2828.5,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1866,1866, 16,     66,    66 }, // 1767: f34GP14

    // Amplitude begins at 3169.6, peaks 4483.5 at 0.0s,
    // fades to 20% at 3.0s, keyoff fades to 20% in 3.0s.
    { 1867,1867,  0,   2973,  2973 }, // 1768: f35GM0; f47GM0; AcouGrandPiano

    // Amplitude begins at 1677.1, peaks 1775.2 at 0.1s,
    // fades to 20% at 3.5s, keyoff fades to 20% in 3.5s.
    { 1868,1868,  0,   3486,  3486 }, // 1769: f35GM1; BrightAcouGrand

    // Amplitude begins at  885.3, peaks  940.4 at 0.0s,
    // fades to 20% at 1.0s, keyoff fades to 20% in 1.0s.
    { 1869,1869,  0,    960,   960 }, // 1770: f35GM6; Harpsichord

    // Amplitude begins at 2289.1,
    // fades to 20% at 1.8s, keyoff fades to 20% in 1.8s.
    { 1870,1870,  0,   1820,  1820 }, // 1771: f35GM7; Clavinet

    // Amplitude begins at 2999.3, peaks 3227.0 at 0.0s,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.9s.
    { 1871,1871,  0,    926,   926 }, // 1772: f35GM9; Glockenspiel

    // Amplitude begins at 2978.4, peaks 3030.5 at 0.0s,
    // fades to 20% at 1.9s, keyoff fades to 20% in 1.9s.
    { 1872,1872,  0,   1886,  1886 }, // 1773: f35GM11; Vibraphone

    // Amplitude begins at  969.7,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 1873,1873,  0,    286,   286 }, // 1774: f35GM12; Marimba

    // Amplitude begins at 1734.2, peaks 2294.0 at 0.0s,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 1874,1874,  0,    446,   446 }, // 1775: f35GM15; Dulcimer

    // Amplitude begins at 1233.8, peaks 1329.8 at 6.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.8s.
    { 1875,1875,  0,  40000,  1840 }, // 1776: f35GM18; Rock Organ

    // Amplitude begins at 1665.7, peaks 1678.5 at 8.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.4s.
    { 1876,1876,  0,  40000,   386 }, // 1777: f35GM19; Church Organ

    // Amplitude begins at 1318.1, peaks 2832.1 at 0.1s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 1877,1877,  0,    340,   340 }, // 1778: f35GM25; Acoustic Guitar2

    // Amplitude begins at 2090.6,
    // fades to 20% at 1.3s, keyoff fades to 20% in 1.3s.
    { 1878,1878,  0,   1266,  1266 }, // 1779: f35GM27; Electric Guitar2

    // Amplitude begins at 1425.6, peaks 2713.1 at 1.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.6s.
    { 1879,1879,  0,  40000,   573 }, // 1780: f35GM31; Guitar Harmonics

    // Amplitude begins at  817.2, peaks 1288.4 at 0.0s,
    // fades to 20% at 2.1s, keyoff fades to 20% in 2.1s.
    { 1880,1880,  0,   2100,  2100 }, // 1781: f35GM32; Acoustic Bass

    // Amplitude begins at 1726.3, peaks 3195.8 at 0.0s,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 1881,1881,  0,    426,   426 }, // 1782: f35GM33; Electric Bass 1

    // Amplitude begins at    1.5, peaks 6181.6 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1882,1882,  0,  40000,   106 }, // 1783: f35GM35; Fretless Bass

    // Amplitude begins at    0.6, peaks  504.3 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 1883,1883,  0,  40000,   193 }, // 1784: f35GM41; Viola

    // Amplitude begins at    0.0, peaks 2358.4 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1884,1884,  0,  40000,   113 }, // 1785: f35GM42; Cello

    // Amplitude begins at 1486.6,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1885,1885,  0,     33,    33 }, // 1786: f35GM45; Pizzicato String

    // Amplitude begins at 2448.6, peaks 3945.2 at 0.0s,
    // fades to 20% at 1.5s, keyoff fades to 20% in 1.5s.
    { 1886,1886,  0,   1466,  1466 }, // 1787: f35GM46; Orchestral Harp

    // Amplitude begins at 2809.7, peaks 3301.0 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 1887,1887,  0,    260,   260 }, // 1788: f35GM47; Timpany

    // Amplitude begins at   74.0, peaks 2492.5 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.0s.
    { 1888,1888,  0,  40000,  1033 }, // 1789: f35GM48; String Ensemble1

    // Amplitude begins at 1580.8, peaks 3321.2 at 2.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.3s.
    { 1889,1889,  0,  40000,   300 }, // 1790: f35GM49; String Ensemble2

    // Amplitude begins at   83.2, peaks  498.9 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 1890,1890,  0,  40000,   193 }, // 1791: f35GM50; Synth Strings 1

    // Amplitude begins at   78.1, peaks  578.0 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1891,1891,  0,  40000,   133 }, // 1792: f35GM51; SynthStrings 2

    // Amplitude begins at  615.0, peaks 1713.7 at 0.0s,
    // fades to 20% at 0.5s, keyoff fades to 20% in 0.5s.
    { 1892,1892,  0,    473,   473 }, // 1793: f35GM55; Orchestra Hit

    // Amplitude begins at   36.3, peaks  849.9 at 0.1s,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.9s.
    { 1893,1893,  0,    913,   913 }, // 1794: f35GM58; f47GM58; Tuba

    // Amplitude begins at    0.0, peaks 2607.5 at 0.3s,
    // fades to 20% at 1.1s, keyoff fades to 20% in 1.1s.
    { 1894,1894,  0,   1133,  1133 }, // 1795: f35GM60; French Horn

    // Amplitude begins at   55.1, peaks 2879.6 at 0.3s,
    // fades to 20% at 3.6s, keyoff fades to 20% in 3.6s.
    { 1895,1895,  0,   3566,  3566 }, // 1796: f35GM62; Synth Brass 1

    // Amplitude begins at   46.2, peaks 2205.8 at 0.1s,
    // fades to 20% at 2.0s, keyoff fades to 20% in 2.0s.
    { 1896,1896,  0,   2000,  2000 }, // 1797: f35GM63; Synth Brass 2

    // Amplitude begins at   69.0, peaks 3241.9 at 24.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1897,1897,  0,  40000,     0 }, // 1798: f35GM68; Oboe

    // Amplitude begins at  336.2, peaks 3558.7 at 0.1s,
    // fades to 20% at 2.7s, keyoff fades to 20% in 2.7s.
    { 1898,1898,  0,   2726,  2726 }, // 1799: f35GM77; Shakuhachi

    // Amplitude begins at   63.0, peaks 2911.6 at 0.1s,
    // fades to 20% at 1.9s, keyoff fades to 20% in 1.9s.
    { 1899,1899,  0,   1940,  1940 }, // 1800: f35GM78; Whistle

    // Amplitude begins at  960.4, peaks 2824.5 at 0.0s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.0s.
    { 1900,1900,  0,    566,    13 }, // 1801: f35GM90; Pad 3 polysynth

    // Amplitude begins at  677.4, peaks 1081.0 at 0.0s,
    // fades to 20% at 1.7s, keyoff fades to 20% in 1.7s.
    { 1901,1901,  0,   1660,  1660 }, // 1802: f35GM96; FX 1 rain

    // Amplitude begins at    0.0, peaks 2774.6 at 2.4s,
    // fades to 20% at 4.7s, keyoff fades to 20% in 4.7s.
    { 1902,1902,  0,   4660,  4660 }, // 1803: f35GM97; FX 2 soundtrack

    // Amplitude begins at 1042.9, peaks 1084.6 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.0s.
    { 1903,1903,  0,    240,     6 }, // 1804: f35GM99; FX 4 atmosphere

    // Amplitude begins at 1544.3, peaks 2887.3 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.1s.
    { 1904,1904,  0,  40000,  1140 }, // 1805: f35GM105; Banjo

    // Amplitude begins at  906.4,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 1905,1905,  0,    313,   313 }, // 1806: f35GM106; Shamisen

    // Amplitude begins at    0.0, peaks  873.8 at 2.3s,
    // fades to 20% at 3.5s, keyoff fades to 20% in 3.5s.
    { 1906,1906,  0,   3540,  3540 }, // 1807: f35GM122; Seashore

    // Amplitude begins at 1727.5, peaks 1971.7 at 0.0s,
    // fades to 20% at 2.0s, keyoff fades to 20% in 2.0s.
    { 1907,1907,  0,   1966,  1966 }, // 1808: f35GM124; Telephone

    // Amplitude begins at    0.0, peaks  792.4 at 2.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 3.2s.
    { 1908,1908,  0,  40000,  3200 }, // 1809: f35GM125; Helicopter

    // Amplitude begins at  881.7,
    // fades to 20% at 1.4s, keyoff fades to 20% in 1.4s.
    { 1909,1909,  0,   1373,  1373 }, // 1810: f35GM127; Gunshot

    // Amplitude begins at 3014.0, peaks 3148.8 at 0.1s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 1910,1910,  1,    626,   626 }, // 1811: f35GP31; f35GP32

    // Amplitude begins at 1939.3,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1911,1911,  8,     13,    13 }, // 1812: f35GP33; f35GP34

    // Amplitude begins at 1531.8, peaks 2009.9 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 326,326,  0,    126,   126 }, // 1813: f35GP36; Bass Drum 1

    // Amplitude begins at 1413.9,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 327,327, 21,     13,    13 }, // 1814: f35GP37; Side Stick

    // Amplitude begins at  848.0,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 328,328, 30,    100,   100 }, // 1815: f35GP38; Acoustic Snare

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 329,329,198,      0,     0 }, // 1816: f35GP39; Hand Clap

    // Amplitude begins at  364.3,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 330,330,  6,     46,    46 }, // 1817: f35GP40; Electric Snare

    // Amplitude begins at  787.1, peaks 3182.2 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 331,331, 14,  40000,     0 }, // 1818: f35GP42; Closed High Hat

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 332,332,196,      0,     0 }, // 1819: f35GP46; Open High Hat

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 335,335,238,      0,     0 }, // 1820: f35GP54; Tambourine

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 324,324,198,      0,     0 }, // 1821: f35GP56; Cow Bell

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1912,1912,224,      0,     0 }, // 1822: f35GP57; Crash Cymbal 2

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 358,358,194,      0,     0 }, // 1823: f35GP60; f35GP61; High Bongo; Low Bongo

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 325,325,192,      0,     0 }, // 1824: f35GP62; Mute High Conga

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 357,357,192,      0,     0 }, // 1825: f35GP63; f35GP64; Low Conga; Open High Conga

    // Amplitude begins at    0.3, peaks  346.1 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1913,1913,101,     93,    93 }, // 1826: f35GP69; Cabasa

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 356,356,194,      0,     0 }, // 1827: f35GP74; Long Guiro

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 336,336,214,      0,     0 }, // 1828: f35GP75; Claves

    // Amplitude begins at  452.6,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1914,1915,  1,     40,    40 }, // 1829: f36GP35; f36GP36; Ac Bass Drum; Bass Drum 1

    // Amplitude begins at 2531.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1916,1917, 18,     33,    33 }, // 1830: f36GP37; Side Stick

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1918,837,128,      0,     0 }, // 1831: f36GP38; f36GP40; Acoustic Snare; Electric Snare

    // Amplitude begins at 1046.7, peaks 1856.7 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 892,892,  6,     53,    53 }, // 1832: f36GP39; f36GP75; f36GP76; f36GP77; f36GP85; Castanets; Claves; Hand Clap; High Wood Block; Low Wood Block

    // Amplitude begins at 1641.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1919,1920,  1,     26,    26 }, // 1833: f36GP41; f36GP42; f36GP43; f36GP44; f36GP45; f36GP46; f36GP47; f36GP48; f36GP49; f36GP50; f36GP51; f36GP52; f36GP53; f36GP59; Chinese Cymbal; Closed High Hat; Crash Cymbal 1; High Floor Tom; High Tom; High-Mid Tom; Low Floor Tom; Low Tom; Low-Mid Tom; Open High Hat; Pedal High Hat; Ride Bell; Ride Cymbal 1; Ride Cymbal 2

    // Amplitude begins at 1433.5, peaks 2663.7 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 855,1921,  6,    133,   133 }, // 1834: f36GP54; Tambourine

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1922,1923,132,      0,     0 }, // 1835: f36GP55; Splash Cymbal

    // Amplitude begins at 1167.5,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 859,859, 17,      6,     6 }, // 1836: f36GP56; Cow Bell

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 863,863,128,      0,     0 }, // 1837: f36GP58; Vibraslap

    // Amplitude begins at  950.4,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1924,1925,  1,     13,    13 }, // 1838: f36GP60; High Bongo

    // Amplitude begins at 2560.8,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1926,1927,  1,     13,    13 }, // 1839: f36GP61; Low Bongo

    // Amplitude begins at 1839.7,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1928,1928,  1,     13,    13 }, // 1840: f36GP62; f36GP86; Mute High Conga; Mute Surdu

    // Amplitude begins at 1874.3,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1929,1929,  1,     20,    20 }, // 1841: f36GP63; f36GP87; Open High Conga; Open Surdu

    // Amplitude begins at 1849.9,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 911,911,  1,     13,    13 }, // 1842: f36GP64; Low Conga

    // Amplitude begins at  826.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1930,1930,  1,     20,    20 }, // 1843: f36GP65; High Timbale

    // Amplitude begins at  792.1,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1931,1931,  0,     80,    80 }, // 1844: f36GP66; Low Timbale

    // Amplitude begins at  781.8,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1932,1932,  3,     46,    46 }, // 1845: f36GP67; High Agogo

    // Amplitude begins at  748.6, peaks  955.6 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1933,1933,  3,     53,    53 }, // 1846: f36GP68; Low Agogo

    // Amplitude begins at    0.2, peaks  360.0 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 881,881, 15,    106,   106 }, // 1847: f36GP69; f36GP82; Cabasa; Shaker

    // Amplitude begins at  137.4, peaks  332.4 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1934,1934, 15,     20,    20 }, // 1848: f36GP70; Maracas

    // Amplitude begins at   61.3, peaks  365.7 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 1935,1935, 87,    266,   266 }, // 1849: f36GP71; Short Whistle

    // Amplitude begins at   67.0, peaks  376.9 at 0.0s,
    // fades to 20% at 0.5s, keyoff fades to 20% in 0.5s.
    { 1936,1936, 87,    526,   526 }, // 1850: f36GP72; Long Whistle

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1937,890,128,      0,     0 }, // 1851: f36GP73; Short Guiro

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1937,891,128,      0,     0 }, // 1852: f36GP74; Long Guiro

    // Amplitude begins at    0.0, peaks 2132.1 at 0.1s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 1938,1939,  1,    153,   153 }, // 1853: f36GP78; Mute Cuica

    // Amplitude begins at    0.2, peaks 1083.7 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1940,1941,  1,     66,    66 }, // 1854: f36GP79; Open Cuica

    // Amplitude begins at 3377.9,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 1942,1943, 34,    160,   160 }, // 1855: f36GP80; Mute Triangle

    // Amplitude begins at 4177.1,
    // fades to 20% at 0.5s, keyoff fades to 20% in 0.5s.
    { 1944,1945, 34,    520,   520 }, // 1856: f36GP81; Open Triangle

    // Amplitude begins at 1531.5, peaks 2367.8 at 0.1s,
    // fades to 20% at 1.0s, keyoff fades to 20% in 1.0s.
    { 1946,1947,  8,    986,   986 }, // 1857: f36GP83; Jingle Bell

    // Amplitude begins at 1383.4, peaks 1458.6 at 0.0s,
    // fades to 20% at 0.8s, keyoff fades to 20% in 0.8s.
    { 1948,1948,  0,    846,   846 }, // 1858: f37GM24; f37GM25; Acoustic Guitar1; Acoustic Guitar2

    // Amplitude begins at 1586.0, peaks 1628.3 at 0.0s,
    // fades to 20% at 0.5s, keyoff fades to 20% in 0.5s.
    { 1949,1950,  0,    520,   520 }, // 1859: f37GM26; Electric Guitar1

    // Amplitude begins at 1492.5, peaks 3378.6 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 1951,1952,  0,  40000,   193 }, // 1860: f37GM40; Violin

    // Amplitude begins at    1.9, peaks  896.7 at 0.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 1953,1953,  0,  40000,   226 }, // 1861: f37GM48; String Ensemble1

    // Amplitude begins at  292.0, peaks 2928.7 at 27.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1954,1955,  0,  40000,    26 }, // 1862: f37GM56; Trumpet

    // Amplitude begins at    2.4, peaks 2848.0 at 0.2s,
    // fades to 20% at 1.4s, keyoff fades to 20% in 1.4s.
    { 1956,1957,  0,   1413,  1413 }, // 1863: f37GM60; French Horn

    // Amplitude begins at 1073.0, peaks 1456.2 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1958,1959,  0,  40000,    40 }, // 1864: f37GM68; f53GM84; Lead 5 charang; Oboe

    // Amplitude begins at  104.2, peaks 2782.4 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1960,1961,  0,  40000,     6 }, // 1865: f37GM69; English Horn

    // Amplitude begins at 1504.9, peaks 3601.7 at 26.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1962,1963,  0,  40000,    46 }, // 1866: f37GM70; Bassoon

    // Amplitude begins at    4.0, peaks 2304.8 at 39.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 1964,1965,  0,  40000,   193 }, // 1867: f37GM74; Recorder

    // Amplitude begins at 2606.3, peaks 3065.3 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1966,1966,  0,     20,    20 }, // 1868: f37GP35; Ac Bass Drum

    // Amplitude begins at  293.9, peaks  575.0 at 3.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1967,1967,  0,  40000,   106 }, // 1869: f41GM3; Honky-tonkPiano

    // Amplitude begins at 2497.5, peaks 3115.6 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 1968,1968,  0,    240,   240 }, // 1870: f41GM4; Rhodes Piano

    // Amplitude begins at 2316.5, peaks 2896.6 at 22.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 1969,1969,  0,  40000,   240 }, // 1871: f41GM6; Harpsichord

    // Amplitude begins at 2220.1, peaks 2506.5 at 30.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 1970,1970,  0,  40000,   153 }, // 1872: f41GM9; Glockenspiel

    // Amplitude begins at 2506.5, peaks 3174.3 at 36.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.5s.
    { 1971,1971,  0,  40000,   473 }, // 1873: f41GM11; Vibraphone

    // Amplitude begins at 1124.0, peaks 1245.6 at 0.0s,
    // fades to 20% at 1.0s, keyoff fades to 20% in 1.0s.
    { 1972,1972,  0,   1046,  1046 }, // 1874: b41M27; f41GM27; Electric Guitar2; nylongtr

    // Amplitude begins at 1679.5, peaks 1741.6 at 0.0s,
    // fades to 20% at 2.9s, keyoff fades to 20% in 2.9s.
    { 1973,1973,  0,   2946,  2946 }, // 1875: b41M47; f41GM47; Timpany; csynth.i

    // Amplitude begins at 1662.2, peaks 3577.7 at 30.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1974,1974,  0,  40000,    46 }, // 1876: f41GM60; French Horn

    // Amplitude begins at  390.5, peaks 2870.9 at 0.0s,
    // fades to 20% at 2.1s, keyoff fades to 20% in 2.1s.
    { 1975,1975,  0,   2133,  2133 }, // 1877: b41M61; f41GM61; Brass Section; elguit3.

    // Amplitude begins at    6.6, peaks 3460.1 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 1976,1976,  0,  40000,   213 }, // 1878: f41GM78; f41GM79; f41GM80; Lead 1 squareea; Ocarina; Whistle

    // Amplitude begins at 1352.3, peaks 1607.5 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1977,1977,  0,  40000,   140 }, // 1879: f41GM96; FX 1 rain

    // Amplitude begins at 2402.9, peaks 3597.0 at 0.0s,
    // fades to 20% at 3.8s, keyoff fades to 20% in 3.8s.
    { 1978,1978,  0,   3786,  3786 }, // 1880: f41GM98; FX 3 crystal

    // Amplitude begins at 2870.4, peaks 2877.9 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 1979,1979,  0,    173,   173 }, // 1881: f41GM115; Woodblock

    // Amplitude begins at 1442.5, peaks 1701.6 at 0.0s,
    // fades to 20% at 2.2s, keyoff fades to 20% in 2.2s.
    { 1980,1980,  0,   2153,  2153 }, // 1882: b42M0; f42GM0; AcouGrandPiano; PIANO1

    // Amplitude begins at 2470.6, peaks 2964.6 at 0.0s,
    // fades to 20% at 1.3s, keyoff fades to 20% in 1.3s.
    { 1981,1981,  0,   1326,  1326 }, // 1883: b42M1; f42GM1; BrightAcouGrand; PIANO2

    // Amplitude begins at 1442.8, peaks 1959.4 at 0.0s,
    // fades to 20% at 1.4s, keyoff fades to 20% in 1.4s.
    { 1982,1982,  0,   1413,  1413 }, // 1884: b42M2; f42GM2; ElecGrandPiano; PIANO3

    // Amplitude begins at 1560.6, peaks 2110.5 at 0.0s,
    // fades to 20% at 0.7s, keyoff fades to 20% in 0.7s.
    { 1983,1983,  0,    660,   660 }, // 1885: b42M3; f42GM3; HONKTONK; Honky-tonkPiano

    // Amplitude begins at 2868.9, peaks 2910.7 at 0.0s,
    // fades to 20% at 1.9s, keyoff fades to 20% in 1.9s.
    { 1984,1984,  0,   1920,  1920 }, // 1886: b42M4; f42GM4; EP1; Rhodes Piano

    // Amplitude begins at 2779.3, peaks 3324.9 at 0.0s,
    // fades to 20% at 1.7s, keyoff fades to 20% in 1.7s.
    { 1985,1985,  0,   1746,  1746 }, // 1887: b42M5; f42GM5; Chorused Piano; EP2

    // Amplitude begins at 1387.7, peaks 1468.7 at 0.0s,
    // fades to 20% at 1.3s, keyoff fades to 20% in 1.3s.
    { 1986,1986,  0,   1273,  1273 }, // 1888: b42M6; f42GM6; HARPSIC; Harpsichord

    // Amplitude begins at 2076.1,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1987,1987,  0,  40000,     0 }, // 1889: b42M7; f42GM7; CLAVIC; Clavinet

    // Amplitude begins at 2012.3, peaks 2138.8 at 0.0s,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    { 1988,1988,  0,   1240,  1240 }, // 1890: b42M8; f42GM8; CELESTA; Celesta

    // Amplitude begins at 2291.3, peaks 2880.0 at 0.0s,
    // fades to 20% at 1.7s, keyoff fades to 20% in 1.7s.
    { 1989,1989,  0,   1660,  1660 }, // 1891: f42GM9; Glockenspiel

    // Amplitude begins at  998.4, peaks 1127.6 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 1990,1990,  0,    293,   293 }, // 1892: b42M10; f42GM10; MUSICBOX; Music box

    // Amplitude begins at    6.9, peaks 2575.4 at 0.0s,
    // fades to 20% at 2.0s, keyoff fades to 20% in 2.0s.
    { 1991,1991,  0,   1993,  1993 }, // 1893: b42M11; f42GM11; VIBES; Vibraphone

    // Amplitude begins at 1662.9, peaks 1868.5 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 1992,1992,  0,    260,   260 }, // 1894: b42M12; f42GM12; MARIMBA; Marimba

    // Amplitude begins at 2594.8, peaks 2632.0 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 1993,1993,  0,    153,   153 }, // 1895: b42M13; f42GM13; XYLO; Xylophone

    // Amplitude begins at 2189.3, peaks 3225.2 at 0.0s,
    // fades to 20% at 2.2s, keyoff fades to 20% in 2.2s.
    { 1994,1994,  0,   2186,  2186 }, // 1896: b42M14; f42GM14; TUBEBELL; Tubular Bells

    // Amplitude begins at 1607.6, peaks 1710.7 at 0.0s,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 1995,1995,  0,    380,   380 }, // 1897: b42M15; f42GM15; Dulcimer; SANTUR

    // Amplitude begins at 1503.9, peaks 1634.0 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1996,1996,  0,  40000,     6 }, // 1898: b42M16; f42GM16; Hammond Organ; ORGAN1

    // Amplitude begins at  955.7, peaks 1119.8 at 6.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1997,1997,  0,  40000,     6 }, // 1899: b42M17; f42GM17; ORGAN2; Percussive Organ

    // Amplitude begins at  849.0, peaks  995.8 at 25.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1998,1998,  0,  40000,    33 }, // 1900: b42M18; f42GM18; ORGAN3; Rock Organ

    // Amplitude begins at    7.4, peaks 1541.2 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 1999,1999,  0,  40000,   120 }, // 1901: b42M19; f42GM19; Church Organ; PIPEORG

    // Amplitude begins at    6.5, peaks 4357.4 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 2000,2000,  0,  40000,    53 }, // 1902: b42M20; f42GM20; REEDORG; Reed Organ

    // Amplitude begins at    0.3, peaks 1234.4 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 2001,2001,  0,  40000,    60 }, // 1903: b42M21; f42GM21; ACORDIAN; Accordion

    // Amplitude begins at  938.6, peaks 2586.0 at 37.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.3s.
    { 2002,2002,  0,  40000,   260 }, // 1904: b42M22; f42GM22; HARMONIC; Harmonica

    // Amplitude begins at    0.0, peaks  527.1 at 14.5s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 2003,2003,  0,  40000,    73 }, // 1905: b42M23; f42GM23; BANDNEON; Tango Accordion

    // Amplitude begins at 2258.4, peaks 2477.7 at 0.0s,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.9s.
    { 2004,2004,  0,    906,   906 }, // 1906: b42M24; f42GM24; Acoustic Guitar1; NYLONGT

    // Amplitude begins at 3223.9, peaks 3294.3 at 0.0s,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.9s.
    { 2005,2005,  0,    853,   853 }, // 1907: b42M25; f42GM25; Acoustic Guitar2; STEELGT

    // Amplitude begins at  848.2, peaks  906.8 at 0.0s,
    // fades to 20% at 2.0s, keyoff fades to 20% in 2.0s.
    { 2006,2006,  0,   1973,  1973 }, // 1908: b42M26; f42GM26; Electric Guitar1; JAZZGT

    // Amplitude begins at 1952.8, peaks 2274.7 at 0.0s,
    // fades to 20% at 1.4s, keyoff fades to 20% in 0.0s.
    { 2007,2007,  0,   1400,    26 }, // 1909: b42M27; f42GM27; CLEANGT; Electric Guitar2

    // Amplitude begins at  256.8, peaks 2882.7 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2008,2008,  0,    213,   213 }, // 1910: b42M28; f42GM28; Electric Guitar3; MUTEGT

    // Amplitude begins at 1019.4, peaks 1492.1 at 0.0s,
    // fades to 20% at 3.7s, keyoff fades to 20% in 3.7s.
    { 2009,2009,  0,   3653,  3653 }, // 1911: b42M29; f42GM29; OVERDGT; Overdrive Guitar

    // Amplitude begins at  647.0, peaks 1604.7 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 2010,2010,  0,     40,    40 }, // 1912: b42M30; f42GM30; DISTGT; Distorton Guitar

    // Amplitude begins at 1300.0, peaks 1397.7 at 0.0s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 2011,2011,  0,    626,   626 }, // 1913: b42M31; f42GM31; GTHARMS; Guitar Harmonics

    // Amplitude begins at 2097.1, peaks 3913.9 at 0.0s,
    // fades to 20% at 1.6s, keyoff fades to 20% in 1.6s.
    { 2012,2012,  0,   1573,  1573 }, // 1914: b42M32; f42GM32; ACOUBASS; Acoustic Bass

    // Amplitude begins at 2764.6, peaks 3543.6 at 0.1s,
    // fades to 20% at 4.2s, keyoff fades to 20% in 4.2s.
    { 2013,2013,  0,   4160,  4160 }, // 1915: b42M33; f42GM33; Electric Bass 1; FINGBASS

    // Amplitude begins at 1650.2,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.9s.
    { 2014,2014,  0,    926,   926 }, // 1916: b42M34; f42GM34; Electric Bass 2; PICKBASS

    // Amplitude begins at    7.1, peaks 4604.5 at 0.1s,
    // fades to 20% at 3.0s, keyoff fades to 20% in 3.0s.
    { 2015,2015,  0,   3006,  3006 }, // 1917: b42M35; f42GM35; FRETLESS; Fretless Bass

    // Amplitude begins at 2063.2,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 2016,2016,  0,    566,   566 }, // 1918: b42M36; f42GM36; SLAPBAS1; Slap Bass 1

    // Amplitude begins at  895.5, peaks  941.0 at 0.0s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 2017,2017,  0,    646,   646 }, // 1919: f42GM37; Slap Bass 2

    // Amplitude begins at 1976.6,
    // fades to 20% at 1.0s, keyoff fades to 20% in 1.0s.
    { 2018,2018,  0,    986,   986 }, // 1920: b42M38; f42GM38; SYNBASS1; Synth Bass 1

    // Amplitude begins at  493.4, peaks 1456.8 at 0.0s,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    { 2019,2019,  0,   1160,  1160 }, // 1921: b42M39; f42GM39; SYNBASS2; Synth Bass 2

    // Amplitude begins at 1381.0,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2020,2020,  0,  40000,    20 }, // 1922: b42M40; f42GM40; VIOLIN; Violin

    // Amplitude begins at    1.2, peaks  622.2 at 34.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2021,2021,  0,  40000,    33 }, // 1923: b42M41; f42GM41; VIOLA; Viola

    // Amplitude begins at    0.3, peaks 1773.4 at 0.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 2022,2022,  0,  40000,    86 }, // 1924: b42M42; f42GM42; CELLO; Cello

    // Amplitude begins at    2.5, peaks 1046.3 at 0.1s,
    // fades to 20% at 2.2s, keyoff fades to 20% in 2.2s.
    { 2023,2023,  0,   2246,  2246 }, // 1925: b42M43; f42GM43; CONTRAB; Contrabass

    // Amplitude begins at    6.9, peaks 1811.6 at 4.5s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 2024,2024,  0,  40000,   133 }, // 1926: b42M44; f42GM44; TREMSTR; Tremulo Strings

    // Amplitude begins at  703.3,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2025,2025,  0,    206,   206 }, // 1927: b42M45; f42GM45; PIZZ; Pizzicato String

    // Amplitude begins at  775.0, peaks  904.5 at 0.0s,
    // fades to 20% at 0.5s, keyoff fades to 20% in 0.5s.
    { 2026,2026,  0,    486,   486 }, // 1928: b42M46; f42GM46; HARP; Orchestral Harp

    // Amplitude begins at 2100.2, peaks 2304.3 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2027,2027,  0,    233,   233 }, // 1929: b42M47; f42GM47; TIMPANI; Timpany

    // Amplitude begins at    2.1, peaks  896.2 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 2028,2028,  0,  40000,   106 }, // 1930: b42M48; f42GM48; STRINGS; String Ensemble1

    // Amplitude begins at    0.0, peaks  514.3 at 13.8s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.3s.
    { 2029,2029,  0,  40000,   260 }, // 1931: b42M49; f42GM49; SLOWSTR; String Ensemble2

    // Amplitude begins at    0.8, peaks 2178.6 at 0.1s,
    // fades to 20% at 1.2s, keyoff fades to 20% in 0.1s.
    { 2030,2030,  0,   1240,    60 }, // 1932: b42M50; f42GM50; SYNSTR1; Synth Strings 1

    // Amplitude begins at    0.0, peaks  882.7 at 0.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 2031,2031,  0,  40000,    60 }, // 1933: b42M51; f42GM51; SYNSTR2; SynthStrings 2

    // Amplitude begins at    0.4, peaks 1385.3 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2032,2032,  0,  40000,     6 }, // 1934: b42M52; f42GM52; CHOIR; Choir Aahs

    // Amplitude begins at    7.1, peaks 3270.1 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2033,2033,  0,  40000,    46 }, // 1935: b42M53; f42GM53; OOHS; Voice Oohs

    // Amplitude begins at    7.2, peaks 3177.0 at 0.0s,
    // fades to 20% at 1.0s, keyoff fades to 20% in 1.0s.
    { 2034,2034,  0,   1046,  1046 }, // 1936: b42M54; f42GM54; SYNVOX; Synth Voice

    // Amplitude begins at  183.8, peaks 2668.6 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2035,2035,  0,    173,   173 }, // 1937: b42M55; f42GM55; ORCHIT; Orchestra Hit

    // Amplitude begins at  244.3, peaks  868.9 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2036,2036,  0,  40000,    13 }, // 1938: b42M56; f42GM56; TRUMPET; Trumpet

    // Amplitude begins at    2.2, peaks  914.0 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2037,2037,  0,  40000,    13 }, // 1939: b42M57; f42GM57; TROMBONE; Trombone

    // Amplitude begins at   33.3, peaks  924.0 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2038,2038,  0,  40000,    20 }, // 1940: b42M58; f42GM58; TUBA; Tuba

    // Amplitude begins at   65.9, peaks  771.1 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2039,2039,  0,  40000,     0 }, // 1941: b42M59; f42GM59; MUTETRP; Muted Trumpet

    // Amplitude begins at    1.5, peaks 2137.3 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 2040,2040,  0,  40000,    93 }, // 1942: b42M60; f42GM60; FRHORN; French Horn

    // Amplitude begins at  120.5, peaks 2762.7 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 2041,2041,  0,  40000,    73 }, // 1943: b42M61; f42GM61; BRASS1; Brass Section

    // Amplitude begins at   37.2, peaks 2433.2 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 2042,2042,  0,  40000,    66 }, // 1944: b42M62; f42GM62; SYNBRAS1; Synth Brass 1

    // Amplitude begins at    2.2, peaks  890.5 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 2043,2043,  0,  40000,   106 }, // 1945: b42M63; f42GM63; SYNBRAS2; Synth Brass 2

    // Amplitude begins at    7.5, peaks 4187.8 at 39.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2044,2044,  0,  40000,    13 }, // 1946: b42M64; f42GM64; SOPSAX; Soprano Sax

    // Amplitude begins at   31.4, peaks  680.7 at 12.5s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2045,2045,  0,  40000,     6 }, // 1947: b42M65; f42GM65; ALTOSAX; Alto Sax

    // Amplitude begins at 2216.1, peaks 2626.0 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2046,2046,  0,  40000,     0 }, // 1948: b42M66; f42GM66; TENSAX; Tenor Sax

    // Amplitude begins at   62.3, peaks 3663.6 at 33.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.5s.
    { 2047,2047,  0,  40000,   466 }, // 1949: b42M67; f42GM67; BARISAX; Baritone Sax

    // Amplitude begins at  118.2, peaks 2878.1 at 10.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2048,2048,  0,  40000,    20 }, // 1950: b42M68; f42GM68; OBOE; Oboe

    // Amplitude begins at    8.0, peaks 2947.3 at 9.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2049,2049,  0,  40000,     6 }, // 1951: b42M69; f42GM69; ENGLHORN; English Horn

    // Amplitude begins at    0.2, peaks 1218.0 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2050,2050,  0,  40000,     6 }, // 1952: b42M70; f42GM70; BASSOON; Bassoon

    // Amplitude begins at    2.0, peaks 3379.7 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2051,2051,  0,  40000,    20 }, // 1953: b42M71; f42GM71; CLARINET; Clarinet

    // Amplitude begins at 1195.2, peaks 3159.6 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2052,2052,  0,  40000,    20 }, // 1954: b42M72; f42GM72; PICCOLO; Piccolo

    // Amplitude begins at  116.6, peaks 3757.4 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2053,2053,  0,  40000,     6 }, // 1955: b42M73; f42GM73; FLUTE1; Flute

    // Amplitude begins at    0.6, peaks 2889.8 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2054,2054,  0,  40000,    13 }, // 1956: b42M74; f42GM74; RECORDER; Recorder

    // Amplitude begins at  111.1, peaks 3193.7 at 35.7s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2055,2055,  0,  40000,    20 }, // 1957: b42M75; f42GM75; PANFLUTE; Pan Flute

    // Amplitude begins at    0.0, peaks  827.3 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2056,2056,  0,  40000,    20 }, // 1958: b42M76; f42GM76; BOTTLEB; Bottle Blow

    // Amplitude begins at    0.0, peaks  758.7 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2057,2057,  0,  40000,    13 }, // 1959: b42M77; f42GM77; SHAKU; Shakuhachi

    // Amplitude begins at  906.0, peaks 4039.7 at 12.5s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2058,2058,  0,  40000,    20 }, // 1960: b42M78; b44P78; f42GM78; WHISTLE; WHISTLE.; Whistle

    // Amplitude begins at 3235.4, peaks 3791.7 at 16.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 2059,2059,  0,  40000,    53 }, // 1961: b42M79; f42GM79; OCARINA; Ocarina

    // Amplitude begins at  635.2, peaks  933.6 at 0.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2060,2060,  0,  40000,     6 }, // 1962: b42M80; f42GM80; Lead 1 squareea; SQUARWAV

    // Amplitude begins at 1971.2, peaks 2550.5 at 0.0s,
    // fades to 20% at 0.8s, keyoff fades to 20% in 0.1s.
    { 2061,2061,  0,    766,    60 }, // 1963: b42M81; f42GM81; Lead 2 sawtooth; SAWWAV

    // Amplitude begins at    0.0, peaks 2245.5 at 0.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2062,2062,  0,  40000,    20 }, // 1964: b42M82; f42GM82; Lead 3 calliope; SYNCALLI

    // Amplitude begins at  764.4, peaks 2186.6 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2063,2063,  0,  40000,    20 }, // 1965: b42M83; f42GM83; CHIFLEAD; Lead 4 chiff

    // Amplitude begins at 1244.4, peaks 1684.2 at 0.1s,
    // fades to 20% at 2.3s, keyoff fades to 20% in 2.3s.
    { 2064,2064,  0,   2273,  2273 }, // 1966: b42M84; f42GM84; CHARANG; Lead 5 charang

    // Amplitude begins at    6.2, peaks 2295.9 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2065,2065,  0,  40000,     6 }, // 1967: b42M85; f42GM85; Lead 6 voice; SOLOVOX

    // Amplitude begins at   58.6, peaks  764.3 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2066,2066,  0,  40000,    20 }, // 1968: b42M86; f42GM86; FIFTHSAW; Lead 7 fifths

    // Amplitude begins at 2450.7, peaks 2874.5 at 0.1s,
    // fades to 20% at 0.8s, keyoff fades to 20% in 0.8s.
    { 2067,2067,  0,    773,   773 }, // 1969: b42M87; f42GM87; BASSLEAD; Lead 8 brass

    // Amplitude begins at 1322.0, peaks 1522.8 at 0.0s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 2068,2068,  0,    593,   593 }, // 1970: b42M88; f42GM88; FANTASIA; Pad 1 new age

    // Amplitude begins at    0.0, peaks 3966.4 at 31.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 3.0s.
    { 2069,2069,  0,  40000,  2980 }, // 1971: b42M89; f42GM89; Pad 2 warm; WARMPAD

    // Amplitude begins at  941.4, peaks 1099.2 at 0.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 2070,2070,  0,  40000,    86 }, // 1972: b42M90; f42GM90; POLYSYN; Pad 3 polysynth

    // Amplitude begins at  140.3, peaks 2885.1 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2071,2071,  0,  40000,    26 }, // 1973: b42M91; f42GM91; Pad 4 choir; SPACEVOX

    // Amplitude begins at    0.0, peaks 2909.7 at 0.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 2072,2072,  0,  40000,    86 }, // 1974: b42M92; f42GM92; BOWEDGLS; Pad 5 bowedpad

    // Amplitude begins at    0.0, peaks 1170.9 at 0.6s,
    // fades to 20% at 2.7s, keyoff fades to 20% in 2.7s.
    { 2073,2073,  0,   2700,  2700 }, // 1975: b42M93; f42GM93; METALPAD; Pad 6 metallic

    // Amplitude begins at    1.0, peaks 4224.1 at 0.1s,
    // fades to 20% at 1.6s, keyoff fades to 20% in 1.6s.
    { 2074,2074,  0,   1626,  1626 }, // 1976: b42M94; f42GM94; HALOPAD; Pad 7 halo

    // Amplitude begins at    0.0, peaks  991.8 at 1.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 2075,2075,  0,  40000,   106 }, // 1977: b42M95; f42GM95; Pad 8 sweep; SWEEPPAD

    // Amplitude begins at 3474.8, peaks 3720.3 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 2076,2076,  0,  40000,   233 }, // 1978: b42M96; f42GM96; FX 1 rain; ICERAIN

    // Amplitude begins at    0.0, peaks  921.7 at 5.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.3s.
    { 2077,2077,  0,  40000,   260 }, // 1979: b42M97; f42GM97; FX 2 soundtrack; SOUNDTRK

    // Amplitude begins at 2446.8, peaks 3212.7 at 0.0s,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.9s.
    { 2078,2078,  0,    906,   906 }, // 1980: b42M98; f42GM98; CRYSTAL; FX 3 crystal

    // Amplitude begins at 1322.5, peaks 1861.9 at 0.0s,
    // fades to 20% at 1.0s, keyoff fades to 20% in 1.0s.
    { 2079,2079,  0,    960,   960 }, // 1981: b42M99; f42GM99; ATMOSPH; FX 4 atmosphere

    // Amplitude begins at 1452.8,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 2080,2080,  0,  40000,   206 }, // 1982: b42M100; f42GM100; BRIGHT; FX 5 brightness

    // Amplitude begins at    0.0, peaks 1051.4 at 0.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2081,2081,  0,  40000,    46 }, // 1983: b42M101; f42GM101; FX 6 goblins; GOBLIN

    // Amplitude begins at 2670.9, peaks 3541.1 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2082,2082,  0,    206,   206 }, // 1984: b42M102; f42GM102; ECHODROP; FX 7 echoes

    // Amplitude begins at   78.8, peaks  728.2 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 2083,2083,  0,  40000,   193 }, // 1985: b42M103; f42GM103; FX 8 sci-fi; STARTHEM

    // Amplitude begins at  620.6, peaks 3093.4 at 0.0s,
    // fades to 20% at 1.7s, keyoff fades to 20% in 0.0s.
    { 2084,2084,  0,   1693,    13 }, // 1986: b42M104; f42GM104; SITAR; Sitar

    // Amplitude begins at  936.9,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2085,2085,  0,    166,   166 }, // 1987: b42M105; f42GM105; BANJO; Banjo

    // Amplitude begins at  986.3,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 2086,2086,  0,    593,   593 }, // 1988: b42M106; f42GM106; SHAMISEN; Shamisen

    // Amplitude begins at 1922.0,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 2087,2087,  0,    406,   406 }, // 1989: b42M107; f42GM107; KOTO; Koto

    // Amplitude begins at 1676.8, peaks 3210.6 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 2088,2088,  0,    260,   260 }, // 1990: b42M108; f42GM108; KALIMBA; Kalimba

    // Amplitude begins at   47.6, peaks  829.8 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2089,2089,  0,  40000,    26 }, // 1991: b42M109; f42GM109; BAGPIPE; Bagpipe

    // Amplitude begins at    3.2, peaks 1739.0 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2090,2090,  0,  40000,    33 }, // 1992: b42M110; f42GM110; FIDDLE; Fiddle

    // Amplitude begins at    6.6, peaks 2159.4 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2091,2091,  0,  40000,    20 }, // 1993: b42M111; f42GM111; SHANNAI; Shanai

    // Amplitude begins at 3274.8, peaks 3389.4 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.3s.
    { 2092,2092,  0,  40000,   260 }, // 1994: b42M112; f42GM112; TINKLBEL; Tinkle Bell

    // Amplitude begins at  622.6, peaks  782.6 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2093,2093,  0,     86,    86 }, // 1995: f42GM113; Agogo Bells

    // Amplitude begins at  849.4,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2094,2094,  0,    153,   153 }, // 1996: b42M114; f42GM114; STEELDRM; Steel Drums

    // Amplitude begins at 3349.7,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2095,2095,  0,     66,    66 }, // 1997: b42M115; f42GM115; WOODBLOK; Woodblock

    // Amplitude begins at 3052.0, peaks 4005.0 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2096,2096,  0,    206,   206 }, // 1998: b42M116; f42GM116; TAIKO; Taiko Drum

    // Amplitude begins at 2010.4, peaks 2507.5 at 0.1s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2097,2097,  0,    160,   160 }, // 1999: b42M117; f42GM117; MELOTOM; Melodic Tom

    // Amplitude begins at 1383.8, peaks 1592.2 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 2098,2098,  0,    313,   313 }, // 2000: b42M118; f42GM118; SYNDRUM; Synth Drum

    // Amplitude begins at 2211.9, peaks 3326.7 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 2099,2099,  0,    260,   260 }, // 2001: f42GM119; Reverse Cymbal

    // Amplitude begins at 1252.3, peaks 2866.3 at 0.0s,
    // fades to 20% at 2.5s, keyoff fades to 20% in 0.0s.
    { 2100,2100,  0,   2533,     6 }, // 2002: f42GM120; Guitar FretNoise

    // Amplitude begins at 3517.9, peaks 4404.9 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 2101,2101,  0,  40000,    66 }, // 2003: f42GM121; Breath Noise

    // Amplitude begins at 1167.9, peaks 1191.8 at 0.0s,
    // fades to 20% at 1.0s, keyoff fades to 20% in 1.0s.
    { 2102,2102,  0,    960,   960 }, // 2004: f42GM122; Seashore

    // Amplitude begins at    0.0, peaks 1131.3 at 0.2s,
    // fades to 20% at 3.7s, keyoff fades to 20% in 3.7s.
    { 2103,2103,  0,   3673,  3673 }, // 2005: f42GM123; Bird Tweet

    // Amplitude begins at 1274.0, peaks 2284.7 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2104,2104,  0,  40000,    20 }, // 2006: f42GM124; Telephone

    // Amplitude begins at 2206.5, peaks 2937.5 at 0.0s,
    // fades to 20% at 4.2s, keyoff fades to 20% in 0.0s.
    { 2105,2105,  0,   4233,     6 }, // 2007: f42GM125; Helicopter

    // Amplitude begins at  821.3, peaks 1249.9 at 0.0s,
    // fades to 20% at 1.4s, keyoff fades to 20% in 1.4s.
    { 2106,2106,  0,   1420,  1420 }, // 2008: f42GM126; Applause/Noise

    // Amplitude begins at  278.1, peaks  813.5 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 2107,2107,  0,     46,    46 }, // 2009: f42GM127; Gunshot

    // Amplitude begins at  475.1, peaks  720.3 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 2107,2107, 62,     40,    40 }, // 2010: f42GP28; f42GP39; Hand Clap

    // Amplitude begins at   19.7, peaks 1337.5 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2108,2108,  1,     66,    66 }, // 2011: f42GP29; f42GP30

    // Amplitude begins at 2264.8,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 2109,2109, 22,     20,    20 }, // 2012: f42GP31; f42GP37; f42GP85; f42GP86; Castanets; Mute Surdu; Side Stick

    // Amplitude begins at 2524.3,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 2110,2110,  0,     33,    33 }, // 2013: f42GP32

    // Amplitude begins at 3634.7,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2095,2095, 37,     93,    93 }, // 2014: f42GP33; f42GP76; f42GP77; High Wood Block; Low Wood Block

    // Amplitude begins at  547.5, peaks  698.4 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1989,1989,  6,     53,    53 }, // 2015: f42GP34

    // Amplitude begins at 1970.4,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2111,2111,  0,     66,    66 }, // 2016: f42GP35; Ac Bass Drum

    // Amplitude begins at  839.4,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2112,2112, 36,    100,   100 }, // 2017: f42GP38; f42GP40; Acoustic Snare; Electric Snare

    // Amplitude begins at 1402.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 2113,2113, 44,     46,    46 }, // 2018: f42GP42; f42GP44; Closed High Hat; Pedal High Hat

    // Amplitude begins at    4.0, peaks 2025.0 at 0.0s,
    // fades to 20% at 0.7s, keyoff fades to 20% in 0.7s.
    { 2114,2114, 46,    660,   660 }, // 2019: f42GP46; Open High Hat

    // Amplitude begins at  794.0, peaks  814.0 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 2115,2115, 44,    320,   320 }, // 2020: f42GP49; f42GP52; f42GP55; f42GP57; Chinese Cymbal; Crash Cymbal 1; Crash Cymbal 2; Splash Cymbal

    // Amplitude begins at  710.3,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2116,2116, 41,    213,   213 }, // 2021: f42GP51; f42GP53; f42GP59; Ride Bell; Ride Cymbal 1; Ride Cymbal 2

    // Amplitude begins at 1832.3, peaks 3165.7 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2117,2117, 46,    233,   233 }, // 2022: f42GP54; Tambourine

    // Amplitude begins at 1288.1, peaks 1368.3 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2118,2118, 55,     93,    93 }, // 2023: f42GP56; Cow Bell

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 2119,2119,128,      0,     0 }, // 2024: f42GP58; Vibraslap

    // Amplitude begins at  524.2, peaks 2477.1 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 2120,2120, 37,     33,    33 }, // 2025: f42GP60; f42GP62; High Bongo; Mute High Conga

    // Amplitude begins at 2010.0, peaks 2392.5 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2121,2121, 33,     53,    53 }, // 2026: f42GP61; Low Bongo

    // Amplitude begins at  540.8, peaks  822.0 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2122,2122, 36,    193,   193 }, // 2027: f42GP63; f42GP64; Low Conga; Open High Conga

    // Amplitude begins at 1563.6, peaks 1596.9 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2123,2123,  5,    186,   186 }, // 2028: f42GP65; f42GP66; High Timbale; Low Timbale

    // Amplitude begins at  312.0, peaks  836.0 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2093,2093, 28,    160,   160 }, // 2029: f42GP67; f42GP68; High Agogo; Low Agogo

    // Amplitude begins at    0.8, peaks  774.5 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2124,2124, 14,     73,    73 }, // 2030: f42GP70; Maracas

    // Amplitude begins at 1932.8, peaks 2047.9 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.0s.
    { 2125,2125, 32,    180,    13 }, // 2031: f42GP71; Short Whistle

    // Amplitude begins at  633.3, peaks 2909.9 at 0.0s,
    // fades to 20% at 1.0s, keyoff fades to 20% in 1.0s.
    { 2126,2126, 32,   1006,  1006 }, // 2032: f42GP72; Long Whistle

    // Amplitude begins at   21.2, peaks  826.4 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 2127,2127, 32,     40,    40 }, // 2033: f42GP73; Short Guiro

    // Amplitude begins at    0.0, peaks 1465.9 at 0.2s,
    // fades to 20% at 0.8s, keyoff fades to 20% in 0.8s.
    { 2128,2128, 32,    846,   846 }, // 2034: f42GP74; Long Guiro

    // Amplitude begins at 1217.7, peaks 1227.4 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2129,2129, 21,    126,   126 }, // 2035: f42GP75; Claves

    // Amplitude begins at   57.2, peaks 2734.7 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.0s.
    { 2130,2130, 32,    106,    13 }, // 2036: f42GP78; Mute Cuica

    // Amplitude begins at    3.0, peaks 3180.6 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2131,2131, 37,     93,    93 }, // 2037: f42GP79; Open Cuica

    // Amplitude begins at 2882.0, peaks 2970.5 at 0.0s,
    // fades to 20% at 5.3s, keyoff fades to 20% in 5.3s.
    { 2132,2132, 34,   5320,  5320 }, // 2038: f42GP80; Mute Triangle

    // Amplitude begins at 2024.4,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 2133,2133, 38,    360,   360 }, // 2039: f42GP81; f42GP83; f42GP84; Bell Tree; Jingle Bell; Open Triangle

    // Amplitude begins at 2787.3, peaks 2811.0 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2096,2096,  2,    166,   166 }, // 2040: f42GP87; Open Surdu

    // Amplitude begins at  932.7,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    { 2134,2134,  0,   1206,  1206 }, // 2041: f47GM2; ElecGrandPiano

    // Amplitude begins at 4426.5, peaks 5810.1 at 0.0s,
    // fades to 20% at 1.3s, keyoff fades to 20% in 1.3s.
    { 2135,2135,  0,   1326,  1326 }, // 2042: f47GM4; Rhodes Piano

    // Amplitude begins at 2107.0, peaks 2870.0 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2136,2136,  0,  40000,     0 }, // 2043: f47GM6; Harpsichord

    // Amplitude begins at 1271.0,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 2137,2137,  0,    300,   300 }, // 2044: f47GM7; Clavinet

    // Amplitude begins at   66.3, peaks 1316.5 at 0.0s,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    { 2138,2138,  0,   1206,  1206 }, // 2045: f47GM8; Celesta

    // Amplitude begins at 1989.5, peaks 2257.2 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 2.9s.
    { 2139,2139,  0,  40000,  2853 }, // 2046: f47GM14; Tubular Bells

    // Amplitude begins at  307.1, peaks 2462.7 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.1s.
    { 2140,2140,  0,  40000,  1120 }, // 2047: f47GM26; Electric Guitar1

    // Amplitude begins at   73.6, peaks 2406.8 at 0.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.6s.
    { 2141,2141,  0,  40000,   560 }, // 2048: f47GM27; Electric Guitar2

    // Amplitude begins at 1277.5, peaks 1376.5 at 29.8s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2142,2142,  0,  40000,    20 }, // 2049: f47GM28; Electric Guitar3

    // Amplitude begins at 1209.9, peaks 1222.7 at 0.0s,
    // fades to 20% at 1.9s, keyoff fades to 20% in 1.9s.
    { 2143,2143,  0,   1893,  1893 }, // 2050: f47GM32; f47GM33; Acoustic Bass; Electric Bass 1

    // Amplitude begins at 3035.9, peaks 3316.4 at 0.0s,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    { 2144,2144,  0,   1240,  1240 }, // 2051: f47GM35; Fretless Bass

    // Amplitude begins at 2061.7, peaks 2914.2 at 0.0s,
    // fades to 20% at 1.5s, keyoff fades to 20% in 1.5s.
    { 2145,2145,  0,   1466,  1466 }, // 2052: f47GM36; Slap Bass 1

    // Amplitude begins at 1217.7, peaks 2582.6 at 0.1s,
    // fades to 20% at 1.9s, keyoff fades to 20% in 1.9s.
    { 2146,2146,  0,   1853,  1853 }, // 2053: f47GM37; Slap Bass 2

    // Amplitude begins at 2119.9, peaks 3431.8 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 2147,2147,  0,    273,   273 }, // 2054: f47GM38; Synth Bass 1

    // Amplitude begins at  963.7, peaks  996.5 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2148,2148,  0,    226,   226 }, // 2055: f47GM39; Synth Bass 2

    // Amplitude begins at    0.0, peaks 2339.4 at 0.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 2149,2149,  0,  40000,    86 }, // 2056: f47GM40; Violin

    // Amplitude begins at    6.1, peaks 2858.5 at 0.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.5s.
    { 2150,2150,  0,  40000,   513 }, // 2057: f47GM42; Cello

    // Amplitude begins at    0.3, peaks 2598.1 at 0.3s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 2151,2151,  0,    573,   573 }, // 2058: f47GM43; Contrabass

    // Amplitude begins at   73.6, peaks 2449.0 at 0.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.0s.
    { 2152,2152,  0,  40000,  1033 }, // 2059: f47GM44; Tremulo Strings

    // Amplitude begins at 3000.0, peaks 4351.3 at 0.0s,
    // fades to 20% at 0.5s, keyoff fades to 20% in 0.5s.
    { 2153,2153,  0,    526,   526 }, // 2060: f47GM45; Pizzicato String

    // Amplitude begins at 2748.9,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2154,2154,  0,     66,    66 }, // 2061: f47GM47; f53GM118; Synth Drum; Timpany

    // Amplitude begins at   73.6, peaks 2449.7 at 0.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.1s.
    { 2155,2155,  0,  40000,  1120 }, // 2062: f47GM48; String Ensemble1

    // Amplitude begins at 1580.8, peaks 3321.2 at 2.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.6s.
    { 2156,2156,  0,  40000,   580 }, // 2063: f47GM49; String Ensemble2

    // Amplitude begins at 2912.7, peaks 3204.5 at 0.5s,
    // fades to 20% at 4.6s, keyoff fades to 20% in 4.6s.
    { 2157,2157,  0,   4573,  4573 }, // 2064: f47GM50; Synth Strings 1

    // Amplitude begins at   73.6, peaks 2470.2 at 0.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.1s.
    { 2158,2158,  0,  40000,  1120 }, // 2065: f47GM51; SynthStrings 2

    // Amplitude begins at   73.5, peaks 2437.2 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.5s.
    { 2159,2159,  0,  40000,   546 }, // 2066: f47GM52; Choir Aahs

    // Amplitude begins at  841.3, peaks 2460.9 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.0s.
    { 2160,2160,  0,  40000,  1033 }, // 2067: f47GM53; Voice Oohs

    // Amplitude begins at    0.3, peaks 1032.2 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 2161,2161,  0,  40000,    53 }, // 2068: f47GM54; Synth Voice

    // Amplitude begins at    0.3, peaks 1076.3 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 2162,2162,  0,  40000,   113 }, // 2069: f47GM55; Orchestra Hit

    // Amplitude begins at  257.7, peaks  918.4 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2163,2163,  0,  40000,     0 }, // 2070: f47GM56; Trumpet

    // Amplitude begins at  242.9, peaks  872.3 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2164,2164,  0,  40000,     0 }, // 2071: f47GM57; Trombone

    // Amplitude begins at    0.6, peaks 2365.0 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2165,2165,  0,  40000,    40 }, // 2072: f47GM59; Muted Trumpet

    // Amplitude begins at    3.4, peaks 1238.8 at 0.1s,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    { 2166,2166,  0,   1240,  1240 }, // 2073: f47GM65; Alto Sax

    // Amplitude begins at  115.8, peaks 3557.3 at 0.1s,
    // fades to 20% at 3.1s, keyoff fades to 20% in 3.1s.
    { 2167,2167,  0,   3060,  3060 }, // 2074: f47GM66; Tenor Sax

    // Amplitude begins at    0.3, peaks  886.7 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2168,2168,  0,  40000,     6 }, // 2075: f47GM67; Baritone Sax

    // Amplitude begins at    7.6, peaks 2838.4 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.0s.
    { 2169,2169,  0,    113,     6 }, // 2076: f47GM74; f54GM75; Pan Flute; Recorder

    // Amplitude begins at    0.0, peaks  838.3 at 0.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 2170,2170,  0,  40000,   193 }, // 2077: f47GM75; Pan Flute

    // Amplitude begins at 3028.2,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.0s.
    { 2171,2171,  0,     73,     6 }, // 2078: f47GM76; Bottle Blow

    // Amplitude begins at 1466.8, peaks 4939.3 at 0.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2172,2172,  0,  40000,    13 }, // 2079: f47GM83; Lead 4 chiff

    // Amplitude begins at 1318.1, peaks 1465.7 at 0.0s,
    // fades to 20% at 1.2s, keyoff fades to 20% in 0.0s.
    { 2173,2173,  0,   1160,     6 }, // 2080: f47GM84; Lead 5 charang

    // Amplitude begins at 3383.4, peaks 3752.9 at 0.0s,
    // fades to 20% at 1.4s, keyoff fades to 20% in 1.4s.
    { 2174,2174,  0,   1433,  1433 }, // 2081: f47GM88; Pad 1 new age

    // Amplitude begins at    0.0, peaks 2878.9 at 1.2s,
    // fades to 20% at 3.9s, keyoff fades to 20% in 3.9s.
    { 2175,2175,  0,   3866,  3866 }, // 2082: f47GM93; Pad 6 metallic

    // Amplitude begins at    4.3, peaks 2503.8 at 7.5s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.6s.
    { 2176,2176,  0,  40000,   560 }, // 2083: f47GM94; Pad 7 halo

    // Amplitude begins at    0.0, peaks 3725.2 at 2.4s,
    // fades to 20% at 6.3s, keyoff fades to 20% in 0.0s.
    { 2177,2177,  0,   6306,    33 }, // 2084: f47GM101; FX 6 goblins

    // Amplitude begins at    6.1, peaks 3213.6 at 2.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.5s.
    { 2178,2178,  0,  40000,   533 }, // 2085: f47GM110; Fiddle

    // Amplitude begins at    0.0, peaks  856.4 at 1.1s,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    { 2179,2179,  0,   1166,  1166 }, // 2086: f47GM122; Seashore

    // Amplitude begins at  841.9,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 1202,1202, 50,    153,   153 }, // 2087: f47GP10; f47GP11; f47GP12; f47GP13; f47GP14; f47GP15; f47GP16; f47GP17; f47GP18; f47GP19; f47GP20; f47GP21; f47GP22; f47GP23; f47GP24; f47GP25; f47GP26; f47GP27; f47GP62; f47GP63; f47GP64; f47GP7; f47GP8; f47GP9; Low Conga; Mute High Conga; Open High Conga

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 281,281,164,      0,     0 }, // 2088: f47GP28

    // Amplitude begins at  771.4,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 2180,2180,  0,     13,    13 }, // 2089: f47GP33; f47GP37; Side Stick

    // Amplitude begins at  379.3, peaks 1008.4 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 2181,2181, 16,     20,    20 }, // 2090: f47GP36; Bass Drum 1

    // Amplitude begins at  272.2, peaks  771.9 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2182,2182,  0,     53,    53 }, // 2091: f47GP38; Acoustic Snare

    // Amplitude begins at  678.4, peaks  734.8 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2183,2183,  9,    106,   106 }, // 2092: f47GP39; Hand Clap

    // Amplitude begins at  878.7,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 2184,2184,  0,     40,    40 }, // 2093: f47GP40; Electric Snare

    // Amplitude begins at 2625.9, peaks 2680.7 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2185,2185,  1,     66,    66 }, // 2094: f47GP41; f47GP42; f47GP43; f47GP44; f47GP45; f47GP46; f47GP47; f47GP48; f47GP49; f47GP50; f47GP51; f47GP52; f47GP53; Chinese Cymbal; Closed High Hat; Crash Cymbal 1; High Floor Tom; High Tom; High-Mid Tom; Low Floor Tom; Low Tom; Low-Mid Tom; Open High Hat; Pedal High Hat; Ride Bell; Ride Cymbal 1

    // Amplitude begins at  799.7,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2186,2186, 79,    126,   126 }, // 2095: f47GP54; Tambourine

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 2187,2187,158,      0,     0 }, // 2096: f47GP56; f47GP67; f47GP68; Cow Bell; High Agogo; Low Agogo

    // Amplitude begins at  813.0, peaks  845.9 at 0.0s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 2188,2188,  6,    613,   613 }, // 2097: f47GP57; Crash Cymbal 2

    // Amplitude begins at  402.7,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 2189,2189, 47,    566,   566 }, // 2098: f47GP59; Ride Cymbal 2

    // Amplitude begins at 2898.1, peaks 3089.6 at 0.0s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 2190,2190,  1,    626,   626 }, // 2099: f47GP60; f47GP61; High Bongo; Low Bongo

    // Amplitude begins at 1206.2, peaks 1231.1 at 0.0s,
    // fades to 20% at 1.0s, keyoff fades to 20% in 1.0s.
    { 2191,2191, 67,   1020,  1020 }, // 2100: f47GP65; f47GP66; High Timbale; Low Timbale

    // Amplitude begins at  795.7,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 275,275,  6,    200,   200 }, // 2101: f47GP69; f47GP70; Cabasa; Maracas

    // Amplitude begins at  813.0, peaks  845.9 at 0.0s,
    // fades to 20% at 0.7s, keyoff fades to 20% in 0.7s.
    { 273,273,  6,    700,   700 }, // 2102: f47GP71; f47GP72; Long Whistle; Short Whistle

    // Amplitude begins at  919.3, peaks 1556.3 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2192,2192, 48,  40000,     0 }, // 2103: f47GP73; Short Guiro

    // Amplitude begins at 1046.7, peaks 1856.7 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2193,2193,  6,     53,    53 }, // 2104: f47GP75; Claves

    // Amplitude begins at  424.8,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1711,1711,  1,      6,     6 }, // 2105: f47GP86; f47GP87; Mute Surdu; Open Surdu

    // Amplitude begins at  291.7, peaks  622.5 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2194,2194,  6,    140,   140 }, // 2106: f47GP88

    // Amplitude begins at 2781.1, peaks 3461.3 at 0.0s,
    // fades to 20% at 0.7s, keyoff fades to 20% in 0.7s.
    { 2195,2195,  0,    660,   660 }, // 2107: f48GM2; ElecGrandPiano

    // Amplitude begins at    0.0, peaks 3238.2 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.8s.
    { 2196,2196,  0,  40000,   753 }, // 2108: f48GM3; Honky-tonkPiano

    // Amplitude begins at 1438.8,
    // fades to 20% at 1.1s, keyoff fades to 20% in 1.1s.
    { 2197,2197,  0,   1126,  1126 }, // 2109: f48GM7; Clavinet

    // Amplitude begins at 2798.3, peaks 3290.7 at 0.0s,
    // fades to 20% at 2.3s, keyoff fades to 20% in 2.3s.
    { 2198,2198,  0,   2253,  2253 }, // 2110: f48GM8; Celesta

    // Amplitude begins at 1985.4, peaks 2087.5 at 0.0s,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    { 2199,2200,  0,   1180,  1180 }, // 2111: f48GM9; Glockenspiel

    // Amplitude begins at    0.0, peaks 5236.1 at 1.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2201,2202,  0,  40000,    26 }, // 2112: f48GM12; Marimba

    // Amplitude begins at 2976.7, peaks 3033.0 at 0.0s,
    // fades to 20% at 1.7s, keyoff fades to 20% in 1.7s.
    { 2203,2203,  0,   1746,  1746 }, // 2113: f48GM13; Xylophone

    // Amplitude begins at 3619.9, peaks 4410.0 at 0.0s,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.9s.
    { 2204,2205,  0,    946,   946 }, // 2114: f48GM14; Tubular Bells

    // Amplitude begins at 1957.2, peaks 2420.9 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2206,2207,  0,  40000,     0 }, // 2115: f48GM16; Hammond Organ

    // Amplitude begins at 2298.8, peaks 3038.1 at 0.1s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.0s.
    { 631,2208,  0,    160,     6 }, // 2116: f48GM17; Percussive Organ

    // Amplitude begins at 1372.5, peaks 1757.7 at 38.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2209,2209,  0,  40000,     0 }, // 2117: f48GM18; Rock Organ

    // Amplitude begins at 1163.6, peaks 1164.2 at 0.0s,
    // fades to 20% at 2.3s, keyoff fades to 20% in 2.3s.
    { 2210,2210,  0,   2306,  2306 }, // 2118: f48GM20; Reed Organ

    // Amplitude begins at 1054.0, peaks 2840.4 at 28.7s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2211,2212,  0,  40000,     0 }, // 2119: f48GM21; Accordion

    // Amplitude begins at 1163.0, peaks 2158.7 at 0.1s,
    // fades to 20% at 0.7s, keyoff fades to 20% in 0.7s.
    { 2213,2213,  0,    680,   680 }, // 2120: f48GM27; Electric Guitar2

    // Amplitude begins at  751.6, peaks 3648.5 at 0.0s,
    // fades to 20% at 5.3s, keyoff fades to 20% in 5.3s.
    { 2214,656,  0,   5333,  5333 }, // 2121: f48GM29; Overdrive Guitar

    // Amplitude begins at 1043.1, peaks 3410.2 at 0.1s,
    // fades to 20% at 1.3s, keyoff fades to 20% in 1.3s.
    { 2215,2216,  0,   1286,  1286 }, // 2122: f48GM30; Distorton Guitar

    // Amplitude begins at    0.0, peaks 3779.9 at 1.2s,
    // fades to 20% at 4.6s, keyoff fades to 20% in 0.0s.
    { 2217,2217,  0,   4586,    13 }, // 2123: f48GM31; Guitar Harmonics

    // Amplitude begins at 1512.7, peaks 5633.2 at 0.0s,
    // fades to 20% at 2.1s, keyoff fades to 20% in 2.1s.
    { 2218,2218,  0,   2133,  2133 }, // 2124: f48GM32; Acoustic Bass

    // Amplitude begins at 3365.5, peaks 4867.6 at 0.0s,
    // fades to 20% at 2.0s, keyoff fades to 20% in 2.0s.
    { 2219,2220,  0,   1993,  1993 }, // 2125: f48GM33; Electric Bass 1

    // Amplitude begins at 3717.2, peaks 4282.2 at 0.1s,
    // fades to 20% at 1.5s, keyoff fades to 20% in 1.5s.
    { 2221,2221,  0,   1540,  1540 }, // 2126: f48GM34; Electric Bass 2

    // Amplitude begins at  101.2, peaks 5000.0 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2222,2223,  0,  40000,     0 }, // 2127: f48GM35; Fretless Bass

    // Amplitude begins at  262.8, peaks 4008.7 at 0.0s,
    // fades to 20% at 2.9s, keyoff fades to 20% in 2.9s.
    { 2224,2224,  0,   2920,  2920 }, // 2128: f48GM36; Slap Bass 1

    // Amplitude begins at  999.7, peaks  999.8 at 0.0s,
    // fades to 20% at 1.9s, keyoff fades to 20% in 1.9s.
    { 2225,2225,  0,   1946,  1946 }, // 2129: f48GM37; Slap Bass 2

    // Amplitude begins at 3787.6,
    // fades to 20% at 0.5s, keyoff fades to 20% in 0.5s.
    { 2226,2226,  0,    540,   540 }, // 2130: f48GM38; Synth Bass 1

    // Amplitude begins at  822.7, peaks  833.9 at 0.0s,
    // fades to 20% at 1.9s, keyoff fades to 20% in 1.9s.
    { 2227,2227,  0,   1940,  1940 }, // 2131: f48GM39; Synth Bass 2

    // Amplitude begins at    7.0, peaks 2868.6 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2228,2229,  0,  40000,     0 }, // 2132: f48GM41; Viola

    // Amplitude begins at 1097.5, peaks 2325.6 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 2230,2231,  0,  40000,   106 }, // 2133: f48GM42; Cello

    // Amplitude begins at    0.6, peaks 1874.8 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 2232,2232,  0,  40000,   126 }, // 2134: f48GM44; Tremulo Strings

    // Amplitude begins at 1035.5, peaks 3110.9 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 677,2233,  0,  40000,   106 }, // 2135: f48GM45; Pizzicato String

    // Amplitude begins at 2061.4, peaks 2786.9 at 0.1s,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.9s.
    { 2234,2234,  0,    906,   906 }, // 2136: f48GM47; Timpany

    // Amplitude begins at    0.0, peaks  734.2 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 687,2235,  0,  40000,    60 }, // 2137: f48GM48; String Ensemble1

    // Amplitude begins at    0.0, peaks  804.5 at 14.8s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2236,2236,  0,  40000,    20 }, // 2138: f48GM49; String Ensemble2

    // Amplitude begins at    5.8, peaks 2094.7 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 2237,2237,  0,  40000,    60 }, // 2139: f48GM52; Choir Aahs

    // Amplitude begins at    0.0, peaks  987.4 at 1.2s,
    // fades to 20% at 3.5s, keyoff fades to 20% in 3.5s.
    { 2238,2239,  0,   3460,  3460 }, // 2140: f48GM53; Voice Oohs

    // Amplitude begins at  492.6, peaks  607.6 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2240,2241,  0,    166,   166 }, // 2141: f48GM55; Orchestra Hit

    // Amplitude begins at  535.3, peaks 2440.1 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2242,2243,  0,  40000,     6 }, // 2142: f48GM56; Trumpet

    // Amplitude begins at  555.3, peaks 2190.6 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2244,2245,  0,  40000,     6 }, // 2143: f48GM57; Trombone

    // Amplitude begins at   42.1, peaks 1075.0 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2246,2246,  0,  40000,    26 }, // 2144: f48GM58; Tuba

    // Amplitude begins at   42.4, peaks  878.2 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2247,2247,  0,  40000,    20 }, // 2145: f48GM59; Muted Trumpet

    // Amplitude begins at  280.0, peaks 5985.3 at 33.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2248,2249,  0,  40000,     6 }, // 2146: f48GM60; French Horn

    // Amplitude begins at   85.4, peaks 3641.9 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2250,2251,  0,  40000,     0 }, // 2147: f48GM61; Brass Section

    // Amplitude begins at  896.8, peaks 1489.5 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2252,2252,  0,  40000,     0 }, // 2148: f48GM63; Synth Brass 2

    // Amplitude begins at  337.4, peaks 1677.9 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2253,2254,  0,  40000,     6 }, // 2149: f48GM64; Soprano Sax

    // Amplitude begins at 2680.4, peaks 3836.9 at 38.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2255,2256,  0,  40000,     6 }, // 2150: f48GM65; Alto Sax

    // Amplitude begins at 2360.8, peaks 5023.2 at 38.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 2257,2258,  0,  40000,    93 }, // 2151: f48GM66; Tenor Sax

    // Amplitude begins at 1117.9, peaks 1547.9 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2259,2260,  0,  40000,    46 }, // 2152: f48GM68; Oboe

    // Amplitude begins at   17.9, peaks 1705.0 at 30.8s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2261,2261,  0,  40000,     6 }, // 2153: f48GM69; English Horn

    // Amplitude begins at    3.1, peaks 1249.5 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2262,2262,  0,  40000,     6 }, // 2154: f48GM70; Bassoon

    // Amplitude begins at    4.5, peaks 1934.6 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2263,2263,  0,  40000,    33 }, // 2155: f48GM71; Clarinet

    // Amplitude begins at   84.8, peaks 4333.1 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2264,2265,  0,  40000,     0 }, // 2156: f48GM72; Piccolo

    // Amplitude begins at    5.9, peaks 2771.0 at 38.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2266,2266,  0,  40000,     0 }, // 2157: f48GM73; Flute

    // Amplitude begins at  633.9, peaks 1164.8 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2267,2268,  0,  40000,     6 }, // 2158: f48GM75; Pan Flute

    // Amplitude begins at    0.0, peaks 3017.1 at 0.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2269,2269,  0,  40000,    20 }, // 2159: f48GM77; Shakuhachi

    // Amplitude begins at 1679.1, peaks 2511.4 at 16.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2270,2270,  0,  40000,     0 }, // 2160: f48GM80; Lead 1 squareea

    // Amplitude begins at 1486.7, peaks 1649.0 at 0.0s,
    // fades to 20% at 2.4s, keyoff fades to 20% in 2.4s.
    { 2271,2271,  0,   2360,  2360 }, // 2161: f48GM81; Lead 2 sawtooth

    // Amplitude begins at  348.3, peaks 1251.9 at 0.0s,
    // fades to 20% at 1.0s, keyoff fades to 20% in 1.0s.
    { 2272,2272,  0,    960,   960 }, // 2162: f48GM82; Lead 3 calliope

    // Amplitude begins at 2038.5, peaks 2946.1 at 9.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2273,2274,  0,  40000,     6 }, // 2163: f48GM85; Lead 6 voice

    // Amplitude begins at 1819.9, peaks 4126.0 at 3.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2275,2276,  0,  40000,     0 }, // 2164: f48GM86; Lead 7 fifths

    // Amplitude begins at 3138.0,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 2277,2278,  0,    306,   306 }, // 2165: f48GM87; Lead 8 brass

    // Amplitude begins at 2312.6, peaks 6840.3 at 0.7s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.7s.
    { 2279,2280,  0,  40000,   733 }, // 2166: f48GM89; Pad 2 warm

    // Amplitude begins at  874.6, peaks 4344.4 at 2.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 2281,2282,  0,  40000,   246 }, // 2167: f48GM90; Pad 3 polysynth

    // Amplitude begins at    0.0, peaks 1473.7 at 0.6s,
    // fades to 20% at 4.9s, keyoff fades to 20% in 0.1s.
    { 2283,2284,  0,   4880,    60 }, // 2168: f48GM92; Pad 5 bowedpad

    // Amplitude begins at    0.6, peaks 2856.2 at 0.2s,
    // fades to 20% at 3.6s, keyoff fades to 20% in 0.1s.
    { 2285,2286,  0,   3586,    80 }, // 2169: f48GM93; Pad 6 metallic

    // Amplitude begins at    0.0, peaks 3048.1 at 2.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 2287,2288,  0,  40000,   160 }, // 2170: f48GM94; Pad 7 halo

    // Amplitude begins at 1269.9, peaks 3285.2 at 0.0s,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.9s.
    { 778,2289,  0,    926,   926 }, // 2171: f48GM96; FX 1 rain

    // Amplitude begins at    0.0, peaks 2723.6 at 0.6s,
    // fades to 20% at 0.8s, keyoff fades to 20% in 0.2s.
    { 2290,2291,  0,    846,   200 }, // 2172: f48GM97; FX 2 soundtrack

    // Amplitude begins at  940.0, peaks 1007.2 at 0.1s,
    // fades to 20% at 2.3s, keyoff fades to 20% in 0.0s.
    { 2292,2292,  0,   2253,     6 }, // 2173: f48GM99; FX 4 atmosphere

    // Amplitude begins at    0.0, peaks 1707.6 at 2.4s,
    // fades to 20% at 7.4s, keyoff fades to 20% in 0.0s.
    { 2293,2294,  0,   7380,    13 }, // 2174: f48GM101; FX 6 goblins

    // Amplitude begins at    0.0, peaks 1998.9 at 2.7s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.3s.
    { 2295,2295,  0,  40000,   260 }, // 2175: f48GM102; FX 7 echoes

    // Amplitude begins at 3278.6, peaks 4234.7 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2296,2296,  0,    206,   206 }, // 2176: f48GM106; Shamisen

    // Amplitude begins at 4216.8,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2297,2298,  0,    246,   246 }, // 2177: f48GM107; Koto

    // Amplitude begins at    0.0, peaks 1197.1 at 1.2s,
    // fades to 20% at 3.4s, keyoff fades to 20% in 3.4s.
    { 2299,2300,  0,   3373,  3373 }, // 2178: f48GM115; Woodblock

    // Amplitude begins at 1553.4, peaks 3615.3 at 22.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 2301,2301,  0,  40000,   106 }, // 2179: f48GM116; Taiko Drum

    // Amplitude begins at 2400.6,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2302,2303,  0,     80,    80 }, // 2180: f48GM117; Melodic Tom

    // Amplitude begins at    0.0, peaks  577.3 at 0.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2304,2304,  0,  40000,     0 }, // 2181: f48GM119; Reverse Cymbal

    // Amplitude begins at 1378.4,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2305,2306,  0,  40000,     0 }, // 2182: f48GM120; Guitar FretNoise

    // Amplitude begins at    0.0, peaks  477.3 at 0.3s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 2307,2307,  0,    586,   586 }, // 2183: f48GM121; Breath Noise

    // Amplitude begins at 1220.6, peaks 4212.0 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 2308,2308,  0,  40000,    73 }, // 2184: f48GM122; Seashore

    // Amplitude begins at 1025.6, peaks 1857.1 at 0.3s,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 2309,2310,  0,    446,   446 }, // 2185: f48GM123; Bird Tweet

    // Amplitude begins at    0.0, peaks 1609.4 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 2311,2311,  0,  40000,   140 }, // 2186: f48GM124; Telephone

    // Amplitude begins at    0.0, peaks 1836.7 at 36.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2312,2313,  0,  40000,     6 }, // 2187: f48GM125; Helicopter

    // Amplitude begins at 1522.1, peaks 2821.7 at 0.1s,
    // fades to 20% at 1.0s, keyoff fades to 20% in 0.0s.
    { 2314,2315,  0,    960,     6 }, // 2188: f48GM126; Applause/Noise

    // Amplitude begins at   49.7, peaks 3504.1 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2316,2316,  0,  40000,     6 }, // 2189: f48GM127; Gunshot

    // Amplitude begins at 1363.3,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 2317,2317, 32,     40,    40 }, // 2190: f48GP35; Ac Bass Drum

    // Amplitude begins at 1410.1,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 2318,2318, 16,     40,    40 }, // 2191: f48GP36; Bass Drum 1

    // Amplitude begins at 1527.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 2319,2319, 51,     20,    20 }, // 2192: f48GP37; Side Stick

    // Amplitude begins at  918.0,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2320,2320,  0,     80,    80 }, // 2193: f48GP38; Acoustic Snare

    // Amplitude begins at 2164.3,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2321,2321, 18,     53,    53 }, // 2194: f48GP39; Hand Clap

    // Amplitude begins at  991.8,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2322,2322, 16,    166,   166 }, // 2195: f48GP40; Electric Snare

    // Amplitude begins at 1903.5,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 2323,2323, 32,     26,    26 }, // 2196: f48GP41; f48GP43; High Floor Tom; Low Floor Tom

    // Amplitude begins at  786.3,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2324,2324, 14,    100,   100 }, // 2197: f48GP42; Closed High Hat

    // Amplitude begins at    0.9, peaks  479.0 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 844,844, 12,     66,    66 }, // 2198: f48GP44; Pedal High Hat

    // Amplitude begins at 1742.4, peaks 2097.9 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 2325,2325,  0,     33,    33 }, // 2199: f48GP45; f48GP47; f48GP48; f48GP50; High Tom; High-Mid Tom; Low Tom; Low-Mid Tom

    // Amplitude begins at 1269.6, peaks 1501.1 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2326,2326, 18,  40000,     0 }, // 2200: f48GP46; Open High Hat

    // Amplitude begins at  185.1, peaks  449.9 at 0.0s,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 2327,2327, 14,    353,   353 }, // 2201: f48GP49; Crash Cymbal 1

    // Amplitude begins at 1059.3,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2328,2328, 46,  40000,     0 }, // 2202: f48GP51; Ride Cymbal 1

    // Amplitude begins at  301.2, peaks  862.7 at 0.0s,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    { 2329,2329, 20,   1160,  1160 }, // 2203: f48GP52; Chinese Cymbal

    // Amplitude begins at  952.1,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 2330,2330, 14,    353,   353 }, // 2204: f48GP53; Ride Bell

    // Amplitude begins at  598.6, peaks  657.2 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2331,2331,  2,     53,    53 }, // 2205: f48GP54; Tambourine

    // Amplitude begins at  428.7,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2332,2332,110,     60,    60 }, // 2206: f48GP55; Splash Cymbal

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 2333,2333,206,      0,     0 }, // 2207: f48GP57; Crash Cymbal 2

    // Amplitude begins at  999.2,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2334,2334, 64,    113,   113 }, // 2208: f48GP58; Vibraslap

    // Amplitude begins at  568.7,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2335,2335, 46,  40000,     0 }, // 2209: f48GP59; Ride Cymbal 2

    // Amplitude begins at  776.7,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2336,2336,  3,    186,   186 }, // 2210: f48GP67; High Agogo

    // Amplitude begins at  860.2, peaks 1114.3 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2337,2337,  3,    140,   140 }, // 2211: f48GP68; Low Agogo

    // Amplitude begins at    0.2, peaks  374.5 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2338,2338, 14,     93,    93 }, // 2212: f48GP69; Cabasa

    // Amplitude begins at  134.4, peaks  348.2 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 2339,2339, 14,     40,    40 }, // 2213: f48GP70; Maracas

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 2340,2340,206,      0,     0 }, // 2214: f48GP73; Short Guiro

    // Amplitude begins at    0.0, peaks 1349.2 at 0.2s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2341,2341, 98,    220,   220 }, // 2215: f48GP74; Long Guiro

    // Amplitude begins at  959.9, peaks 1702.6 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2342,2342,  6,     53,    53 }, // 2216: f48GP75; Claves

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 2343,2343,225,      0,     0 }, // 2217: f48GP78; Mute Cuica

    // Amplitude begins at 1392.8,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 2344,2344, 10,    386,   386 }, // 2218: f48GP80; Mute Triangle

    // Amplitude begins at 1433.7, peaks 1443.9 at 0.0s,
    // fades to 20% at 3.0s, keyoff fades to 20% in 3.0s.
    { 2345,2345, 10,   3046,  3046 }, // 2219: f48GP81; Open Triangle

    // Amplitude begins at    3.1, peaks 2810.4 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2346,2346, 14,     60,    60 }, // 2220: f48GP82; Shaker

    // Amplitude begins at 1388.6,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 2347,2347, 37,    366,   366 }, // 2221: f48GP84; Bell Tree

    // Amplitude begins at 1845.5,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 2348,2348,  1,     13,    13 }, // 2222: f48GP86; Mute Surdu

    // Amplitude begins at 1986.7,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 2349,2349,  0,     40,    40 }, // 2223: f48GP87; Open Surdu

    // Amplitude begins at 1317.7, peaks 3266.2 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2350,2351,  0,  40000,     0 }, // 2224: f49GM0; f49GM29; AcouGrandPiano; Overdrive Guitar

    // Amplitude begins at 1640.5, peaks 1895.1 at 5.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.1s.
    { 2352,2353,  0,  40000,  1100 }, // 2225: f49GM2; ElecGrandPiano

    // Amplitude begins at 1765.7, peaks 2131.6 at 0.0s,
    // fades to 20% at 3.6s, keyoff fades to 20% in 3.6s.
    { 2354,2355,  0,   3640,  3640 }, // 2226: f49GM3; Honky-tonkPiano

    // Amplitude begins at 2374.8,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2356,2357,  0,  40000,     0 }, // 2227: f49GM111; f49GM7; Clavinet; Shanai

    // Amplitude begins at  668.1,
    // fades to 20% at 1.9s, keyoff fades to 20% in 1.9s.
    { 2358,2359,  0,   1893,  1893 }, // 2228: f49GM32; f49GM8; Acoustic Bass; Celesta

    // Amplitude begins at 1273.6, peaks 1914.7 at 0.0s,
    // fades to 20% at 4.6s, keyoff fades to 20% in 4.6s.
    { 2360,2361,  0,   4613,  4613 }, // 2229: f49GM24; f49GM9; Acoustic Guitar1; Glockenspiel

    // Amplitude begins at  968.5, peaks 3695.8 at 10.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 633,2362,  0,  40000,     6 }, // 2230: f49GM18; Rock Organ

    // Amplitude begins at 2707.6, peaks 3438.5 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2363,2364,  0,    240,   240 }, // 2231: f49GM27; Electric Guitar2

    // Amplitude begins at 2126.3, peaks 2543.3 at 0.0s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 2365,2366,  0,    566,   566 }, // 2232: f49GM28; Electric Guitar3

    // Amplitude begins at  964.8, peaks 3451.2 at 31.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2367,2368,  0,  40000,     0 }, // 2233: f49GM30; Distorton Guitar

    // Amplitude begins at  668.1, peaks  681.4 at 2.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2369,2370,  0,  40000,     6 }, // 2234: f49GM31; Guitar Harmonics

    // Amplitude begins at  637.5,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2371,2372,  0,  40000,     6 }, // 2235: f49GM33; Electric Bass 1

    // Amplitude begins at 2371.7, peaks 2630.9 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 2373,2374,  0,    320,   320 }, // 2236: f49GM34; Electric Bass 2

    // Amplitude begins at 4124.9, peaks 5626.8 at 0.0s,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 2375,670,  0,    366,   366 }, // 2237: f49GM37; Slap Bass 2

    // Amplitude begins at 1636.3,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2376,2377,  0,    140,   140 }, // 2238: f49GM38; Synth Bass 1

    // Amplitude begins at 1797.8, peaks 3569.8 at 0.1s,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.9s.
    { 2378,2379,  0,    853,   853 }, // 2239: f49GM47; Timpany

    // Amplitude begins at 2323.6, peaks 3762.8 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.8s.
    { 691,2380,  0,  40000,   826 }, // 2240: f49GM50; Synth Strings 1

    // Amplitude begins at  727.2, peaks 1157.8 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2381,2382,  0,  40000,     6 }, // 2241: f49GM75; Pan Flute

    // Amplitude begins at 2169.2,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 750,2383,  0,  40000,     6 }, // 2242: f49GM79; Ocarina

    // Amplitude begins at 1303.2, peaks 1664.9 at 21.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2384,2385,  0,  40000,    13 }, // 2243: f49GM85; Lead 6 voice

    // Amplitude begins at    0.6, peaks 5020.9 at 0.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2386,2387,  0,  40000,     0 }, // 2244: f49GM86; Lead 7 fifths

    // Amplitude begins at  190.9, peaks 3013.8 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 2388,2389,  0,    320,   320 }, // 2245: f49GM87; Lead 8 brass

    // Amplitude begins at    0.6, peaks 5020.9 at 0.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2390,2387,  0,  40000,     0 }, // 2246: f49GM88; Pad 1 new age

    // Amplitude begins at 2050.9, peaks 4465.9 at 2.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 776,2391,  0,  40000,    53 }, // 2247: f49GM95; Pad 8 sweep

    // Amplitude begins at 1300.7,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 2392,2393,  0,    340,   340 }, // 2248: f49GM99; FX 4 atmosphere

    // Amplitude begins at 4216.8,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2394,2395,  0,    246,   246 }, // 2249: f49GM107; Koto

    // Amplitude begins at 1610.4,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2396,2397,  0,    226,   226 }, // 2250: f49GM112; Tinkle Bell

    // Amplitude begins at 1718.2, peaks 2309.2 at 0.0s,
    // fades to 20% at 1.6s, keyoff fades to 20% in 1.6s.
    { 2398,2399,  0,   1613,  1613 }, // 2251: f49GM113; Agogo Bells

    // Amplitude begins at    0.0, peaks  716.8 at 34.5s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2400,2401,  0,  40000,     6 }, // 2252: f49GM119; Reverse Cymbal

    // Amplitude begins at 1113.7,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 2402,2403,  0,    440,   440 }, // 2253: f49GM120; Guitar FretNoise

    // Amplitude begins at 1413.1,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 2404,2405,  0,    300,   300 }, // 2254: f49GM121; Breath Noise

    // Amplitude begins at  971.5,
    // fades to 20% at 0.5s, keyoff fades to 20% in 0.5s.
    { 2406,2407,  0,    500,   500 }, // 2255: f49GM124; Telephone

    // Amplitude begins at  569.8, peaks  611.8 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2408,2409,  0,  40000,     0 }, // 2256: f49GM126; Applause/Noise

    // Amplitude begins at 1352.3,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 2410,2410, 16,     20,    20 }, // 2257: f49GP35; f49GP36; Ac Bass Drum; Bass Drum 1

    // Amplitude begins at  897.1,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2411,2411, 16,    166,   166 }, // 2258: f49GP40; Electric Snare

    // Amplitude begins at  629.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 2412,2412, 14,     46,    46 }, // 2259: f49GP42; Closed High Hat

    // Amplitude begins at  825.3, peaks  861.7 at 0.0s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 2413,2413,  0,    600,   600 }, // 2260: f50GM40; f50GM78; Violin; Whistle

    // Amplitude begins at 4364.5, peaks 4917.7 at 0.0s,
    // fades to 20% at 3.0s, keyoff fades to 20% in 3.0s.
    { 2414,2415,  0,   2973,  2973 }, // 2261: f53GM0; AcouGrandPiano

    // Amplitude begins at 1453.2, peaks 1465.8 at 0.0s,
    // fades to 20% at 1.6s, keyoff fades to 20% in 1.6s.
    { 2416,2417,  0,   1586,  1586 }, // 2262: f53GM1; BrightAcouGrand

    // Amplitude begins at 3156.7, peaks 3727.4 at 0.0s,
    // fades to 20% at 8.6s, keyoff fades to 20% in 0.0s.
    { 2418,2419,  0,   8593,    26 }, // 2263: f53GM2; ElecGrandPiano

    // Amplitude begins at 2367.7, peaks 3540.1 at 0.0s,
    // fades to 20% at 2.0s, keyoff fades to 20% in 2.0s.
    { 2420,2421,  0,   1960,  1960 }, // 2264: f53GM3; Honky-tonkPiano

    // Amplitude begins at 5831.4, peaks 6537.1 at 0.0s,
    // fades to 20% at 2.1s, keyoff fades to 20% in 2.1s.
    { 2422,2423,  0,   2080,  2080 }, // 2265: f53GM4; Rhodes Piano

    // Amplitude begins at 1424.6, peaks 1734.5 at 0.0s,
    // fades to 20% at 1.5s, keyoff fades to 20% in 1.5s.
    { 2424,2425,  0,   1486,  1486 }, // 2266: f53GM5; f54GM99; Chorused Piano; FX 4 atmosphere

    // Amplitude begins at 1293.3, peaks 1895.5 at 0.1s,
    // fades to 20% at 3.2s, keyoff fades to 20% in 0.0s.
    { 2426,2427,  0,   3160,    13 }, // 2267: f53GM6; Harpsichord

    // Amplitude begins at 1764.3, peaks 2059.9 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2428,2429,  0,  40000,    13 }, // 2268: f53GM7; Clavinet

    // Amplitude begins at  140.3, peaks 1313.2 at 30.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 2430,2431,  0,  40000,    66 }, // 2269: f53GM8; Celesta

    // Amplitude begins at 3864.8,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2432,2433,  0,  40000,     6 }, // 2270: f53GM9; Glockenspiel

    // Amplitude begins at 2598.9, peaks 2819.2 at 12.7s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2434,2435,  0,  40000,     6 }, // 2271: f53GM10; Music box

    // Amplitude begins at 3286.6, peaks 4222.5 at 34.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2436,2437,  0,  40000,     0 }, // 2272: f53GM11; Vibraphone

    // Amplitude begins at 1730.8, peaks 1830.6 at 36.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2438,2439,  0,  40000,     6 }, // 2273: f53GM12; Marimba

    // Amplitude begins at 1038.1, peaks 1092.4 at 11.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2440,2441,  0,  40000,     0 }, // 2274: f53GM13; Xylophone

    // Amplitude begins at 2916.4, peaks 3258.9 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2442,2443,  0,  40000,     6 }, // 2275: f53GM14; Tubular Bells

    // Amplitude begins at    0.2, peaks 1687.7 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2444,2445,  0,  40000,    20 }, // 2276: f53GM15; Dulcimer

    // Amplitude begins at 4547.6,
    // fades to 20% at 1.7s, keyoff fades to 20% in 1.7s.
    { 2446,2447,  0,   1713,  1713 }, // 2277: f53GM16; Hammond Organ

    // Amplitude begins at 1190.2, peaks 1352.8 at 0.1s,
    // fades to 20% at 2.6s, keyoff fades to 20% in 2.6s.
    { 2448,2449,  0,   2620,  2620 }, // 2278: f53GM17; Percussive Organ

    // Amplitude begins at  945.5, peaks 1180.5 at 18.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.3s.
    { 2450,2451,  0,  40000,   253 }, // 2279: f53GM18; Rock Organ

    // Amplitude begins at 2021.1, peaks 2201.4 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 2452,2453,  0,  40000,   186 }, // 2280: f53GM19; Church Organ

    // Amplitude begins at 2161.7, peaks 2466.6 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 2454,2455,  0,  40000,   133 }, // 2281: f53GM20; Reed Organ

    // Amplitude begins at 3536.3, peaks 4170.0 at 0.0s,
    // fades to 20% at 0.5s, keyoff fades to 20% in 0.5s.
    { 2456,2457,  0,    453,   453 }, // 2282: f53GM21; Accordion

    // Amplitude begins at 4425.0, peaks 5411.7 at 3.1s,
    // fades to 20% at 3.2s, keyoff fades to 20% in 2.0s.
    { 2458,2459,  0,   3173,  2000 }, // 2283: f53GM22; Harmonica

    // Amplitude begins at 4409.9, peaks 6518.0 at 2.2s,
    // fades to 20% at 2.2s, keyoff fades to 20% in 0.1s.
    { 2458,2460,  0,   2206,   126 }, // 2284: f53GM23; Tango Accordion

    // Amplitude begins at    0.0, peaks 1156.0 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2461,2462,  0,  40000,     6 }, // 2285: f53GM24; Acoustic Guitar1

    // Amplitude begins at    0.0, peaks 2886.1 at 0.4s,
    // fades to 20% at 11.8s, keyoff fades to 20% in 0.1s.
    { 2463,2464,  0,  11766,    73 }, // 2286: f53GM25; Acoustic Guitar2

    // Amplitude begins at  902.8, peaks 1356.6 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2465,2466,  0,  40000,     6 }, // 2287: f53GM26; Electric Guitar1

    // Amplitude begins at  851.9, peaks 1491.9 at 0.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2467,2468,  0,  40000,     6 }, // 2288: f53GM27; Electric Guitar2

    // Amplitude begins at 1753.2, peaks 3924.5 at 0.1s,
    // fades to 20% at 1.0s, keyoff fades to 20% in 1.0s.
    { 2469,2470,  0,    966,   966 }, // 2289: f53GM28; Electric Guitar3

    // Amplitude begins at  993.2, peaks 1304.7 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2471,2472,  0,  40000,     0 }, // 2290: f53GM29; Overdrive Guitar

    // Amplitude begins at 1742.4, peaks 2450.4 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2473,2474,  0,    220,   220 }, // 2291: f53GM30; Distorton Guitar

    // Amplitude begins at 2119.2, peaks 3064.6 at 0.1s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 2475,2476,  0,    253,   253 }, // 2292: f53GM31; Guitar Harmonics

    // Amplitude begins at 2192.6, peaks 2662.8 at 0.0s,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.9s.
    { 2477,2478,  0,    933,   933 }, // 2293: f53GM32; Acoustic Bass

    // Amplitude begins at 1132.9, peaks 1302.2 at 0.0s,
    // fades to 20% at 2.3s, keyoff fades to 20% in 2.3s.
    { 2479,2480,  0,   2306,  2306 }, // 2294: f53GM33; Electric Bass 1

    // Amplitude begins at 1490.4, peaks 1574.1 at 0.0s,
    // fades to 20% at 1.9s, keyoff fades to 20% in 1.9s.
    { 2481,2482,  0,   1940,  1940 }, // 2295: f53GM34; Electric Bass 2

    // Amplitude begins at 1446.8, peaks 2336.6 at 0.0s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 2483,2484,  0,    593,   593 }, // 2296: f53GM35; Fretless Bass

    // Amplitude begins at 1809.9,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 2485,2486,  0,  40000,   213 }, // 2297: f53GM36; Slap Bass 1

    // Amplitude begins at   95.3, peaks 2284.8 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2487,2488,  0,    100,   100 }, // 2298: f53GM37; Slap Bass 2

    // Amplitude begins at 2848.3,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 2489,2490,  0,  40000,   220 }, // 2299: f53GM38; Synth Bass 1

    // Amplitude begins at 1418.2, peaks 2588.8 at 19.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 2491,2492,  0,  40000,   160 }, // 2300: f53GM39; Synth Bass 2

    // Amplitude begins at 1401.7, peaks 1435.5 at 0.1s,
    // fades to 20% at 2.4s, keyoff fades to 20% in 2.4s.
    { 2493,2494,  0,   2373,  2373 }, // 2301: f53GM40; Violin

    // Amplitude begins at 2230.0, peaks 2959.3 at 0.0s,
    // fades to 20% at 0.7s, keyoff fades to 20% in 0.7s.
    { 2495,2496,  0,    680,   680 }, // 2302: f53GM41; Viola

    // Amplitude begins at 2337.5, peaks 2890.9 at 0.0s,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.9s.
    { 2497,2498,  0,    906,   906 }, // 2303: f53GM42; Cello

    // Amplitude begins at  835.2, peaks 1692.4 at 0.0s,
    // fades to 20% at 4.0s, keyoff fades to 20% in 0.0s.
    { 2499,2500,  0,   3960,    13 }, // 2304: f53GM43; Contrabass

    // Amplitude begins at 3079.9, peaks 5102.6 at 0.0s,
    // fades to 20% at 2.0s, keyoff fades to 20% in 2.0s.
    { 2501,2502,  0,   1993,  1993 }, // 2305: f53GM44; Tremulo Strings

    // Amplitude begins at 2028.4, peaks 2089.4 at 0.0s,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    { 2503,2504,  0,   1160,  1160 }, // 2306: f53GM45; Pizzicato String

    // Amplitude begins at 1897.9, peaks 1949.9 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2505,2506,  0,  40000,     0 }, // 2307: f53GM46; Orchestral Harp

    // Amplitude begins at 2946.6, peaks 3765.9 at 0.0s,
    // fades to 20% at 1.0s, keyoff fades to 20% in 1.0s.
    { 2507,2508,  0,    980,   980 }, // 2308: f53GM47; Timpany

    // Amplitude begins at  573.4, peaks 1171.1 at 18.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.3s.
    { 2509,2510,  0,  40000,   253 }, // 2309: f53GM48; f53GM51; String Ensemble1; SynthStrings 2

    // Amplitude begins at 1414.2, peaks 2147.1 at 22.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.4s.
    { 2511,2512,  0,  40000,   360 }, // 2310: f53GM49; String Ensemble2

    // Amplitude begins at  677.8, peaks 1129.5 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.1s.
    { 2513,2514,  0,  40000,  1053 }, // 2311: f53GM50; Synth Strings 1

    // Amplitude begins at  884.5, peaks 1520.0 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 2515,2516,  0,  40000,   140 }, // 2312: f53GM52; Choir Aahs

    // Amplitude begins at 2722.7, peaks 3864.8 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.6s.
    { 2517,2518,  0,  40000,   560 }, // 2313: f53GM53; Voice Oohs

    // Amplitude begins at 3380.2, peaks 3692.8 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 2519,2520,  0,  40000,   173 }, // 2314: f53GM54; Synth Voice

    // Amplitude begins at  528.3,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2521,2521,  0,    233,   233 }, // 2315: f53GM55; Orchestra Hit

    // Amplitude begins at  549.5, peaks  555.3 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2522,2522,  0,    213,   213 }, // 2316: f53GM56; Trumpet

    // Amplitude begins at 1249.8,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.9s.
    { 2523,2524,  0,    913,   913 }, // 2317: f53GM57; Trombone

    // Amplitude begins at 1447.7, peaks 1946.8 at 0.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 2525,2526,  0,  40000,   106 }, // 2318: f53GM58; Tuba

    // Amplitude begins at 1565.0,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.9s.
    { 2527,2528,  0,    920,   920 }, // 2319: f53GM59; Muted Trumpet

    // Amplitude begins at  326.5, peaks 1803.8 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2529,2530,  0,  40000,    13 }, // 2320: f53GM60; French Horn

    // Amplitude begins at 1624.9, peaks 2017.9 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2531,2532,  0,  40000,     0 }, // 2321: f53GM61; Brass Section

    // Amplitude begins at 1554.2, peaks 1625.4 at 0.0s,
    // fades to 20% at 1.0s, keyoff fades to 20% in 1.0s.
    { 2533,2534,  0,    960,   960 }, // 2322: f53GM62; Synth Brass 1

    // Amplitude begins at 2925.3, peaks 3142.0 at 0.1s,
    // fades to 20% at 1.7s, keyoff fades to 20% in 1.7s.
    { 2535,2536,  0,   1693,  1693 }, // 2323: f53GM64; f54GM37; Slap Bass 2; Soprano Sax

    // Amplitude begins at 1253.0,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 2537,2538,  0,    640,   640 }, // 2324: f53GM65; Alto Sax

    // Amplitude begins at 1366.8, peaks 3057.9 at 0.1s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 2539,2540,  0,    340,   340 }, // 2325: f53GM66; Tenor Sax

    // Amplitude begins at 3430.3, peaks 3645.7 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2541,2542,  0,    166,   166 }, // 2326: f53GM67; Baritone Sax

    // Amplitude begins at  863.7, peaks 1354.7 at 0.0s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 2543,2544,  0,    626,   626 }, // 2327: f53GM68; Oboe

    // Amplitude begins at 1826.9, peaks 2166.8 at 0.0s,
    // fades to 20% at 4.0s, keyoff fades to 20% in 0.0s.
    { 2545,2546,  0,   3953,     6 }, // 2328: f53GM69; English Horn

    // Amplitude begins at 1094.9,
    // fades to 20% at 0.7s, keyoff fades to 20% in 0.7s.
    { 2547,2548,  0,    693,   693 }, // 2329: f53GM70; Bassoon

    // Amplitude begins at 1668.9, peaks 1832.8 at 0.1s,
    // fades to 20% at 3.6s, keyoff fades to 20% in 3.6s.
    { 2549,2550,  0,   3560,  3560 }, // 2330: f53GM71; Clarinet

    // Amplitude begins at 1470.3, peaks 1650.5 at 22.5s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 2551,2552,  0,  40000,   173 }, // 2331: f53GM72; Piccolo

    // Amplitude begins at   73.2, peaks 2460.8 at 0.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.0s.
    { 2553,2553,  0,  40000,  1033 }, // 2332: f53GM74; Recorder

    // Amplitude begins at   33.2, peaks  969.7 at 0.0s,
    // fades to 20% at 4.0s, keyoff fades to 20% in 4.0s.
    { 2554,2554,  0,   3973,  3973 }, // 2333: f53GM75; Pan Flute

    // Amplitude begins at   33.0, peaks  898.7 at 0.1s,
    // fades to 20% at 4.6s, keyoff fades to 20% in 4.6s.
    { 2555,2555,  0,   4586,  4586 }, // 2334: f53GM76; Bottle Blow

    // Amplitude begins at 1228.7, peaks 2052.7 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 2556,2557,  0,  40000,   113 }, // 2335: f53GM78; Whistle

    // Amplitude begins at  512.3, peaks  849.4 at 0.0s,
    // fades to 20% at 2.3s, keyoff fades to 20% in 2.3s.
    { 2558,2558,  0,   2346,  2346 }, // 2336: f53GM79; Ocarina

    // Amplitude begins at   33.1, peaks  939.3 at 0.1s,
    // fades to 20% at 3.5s, keyoff fades to 20% in 3.5s.
    { 2559,2559,  0,   3513,  3513 }, // 2337: f53GM80; Lead 1 squareea

    // Amplitude begins at 1119.4,
    // fades to 20% at 1.3s, keyoff fades to 20% in 1.3s.
    { 2560,2560,  0,   1273,  1273 }, // 2338: f53GM81; Lead 2 sawtooth

    // Amplitude begins at    0.0, peaks 2317.2 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 2561,2562,  0,  40000,   100 }, // 2339: f53GM83; Lead 4 chiff

    // Amplitude begins at  949.9, peaks 2851.0 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2563,2564,  0,  40000,    13 }, // 2340: f53GM85; Lead 6 voice

    // Amplitude begins at 1504.9, peaks 3661.6 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 1962,2565,  0,  40000,    46 }, // 2341: f53GM86; Lead 7 fifths

    // Amplitude begins at  258.9, peaks 3305.2 at 0.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.5s.
    { 2566,2566,  0,  40000,  1506 }, // 2342: f53GM87; Lead 8 brass

    // Amplitude begins at  258.9, peaks  875.2 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2567,2567,  0,  40000,     0 }, // 2343: f53GM88; Pad 1 new age

    // Amplitude begins at    4.3, peaks 3167.9 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2568,2569,  0,  40000,    20 }, // 2344: f53GM90; Pad 3 polysynth

    // Amplitude begins at   38.0, peaks 2005.9 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2570,2571,  0,  40000,    33 }, // 2345: f53GM91; Pad 4 choir

    // Amplitude begins at    4.3, peaks 2074.1 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2572,2573,  0,  40000,     6 }, // 2346: f53GM92; Pad 5 bowedpad

    // Amplitude begins at    1.9, peaks 2134.5 at 0.5s,
    // fades to 20% at 6.3s, keyoff fades to 20% in 0.0s.
    { 2574,2575,  0,   6280,    13 }, // 2347: f53GM94; Pad 7 halo

    // Amplitude begins at  734.1, peaks 1202.6 at 39.8s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 2576,2577,  0,  40000,   173 }, // 2348: f53GM95; Pad 8 sweep

    // Amplitude begins at  258.9, peaks  870.6 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2578,2578,  0,  40000,     0 }, // 2349: f53GM96; FX 1 rain

    // Amplitude begins at 2822.4, peaks 3102.9 at 0.0s,
    // fades to 20% at 1.4s, keyoff fades to 20% in 1.4s.
    { 2579,2580,  0,   1420,  1420 }, // 2350: f53GM97; FX 2 soundtrack

    // Amplitude begins at 4076.8, peaks 4107.7 at 0.0s,
    // fades to 20% at 1.5s, keyoff fades to 20% in 1.5s.
    { 2581,2582,  0,   1486,  1486 }, // 2351: f53GM98; FX 3 crystal

    // Amplitude begins at 3995.7,
    // fades to 20% at 1.7s, keyoff fades to 20% in 1.7s.
    { 2583,2584,  0,   1746,  1746 }, // 2352: f53GM99; FX 4 atmosphere

    // Amplitude begins at 3006.7, peaks 3058.5 at 0.0s,
    // fades to 20% at 1.3s, keyoff fades to 20% in 1.3s.
    { 2585,2586,  0,   1346,  1346 }, // 2353: f53GM100; FX 5 brightness

    // Amplitude begins at 3949.8, peaks 4105.9 at 0.0s,
    // fades to 20% at 2.3s, keyoff fades to 20% in 2.3s.
    { 2587,2588,  0,   2306,  2306 }, // 2354: f53GM101; FX 6 goblins

    // Amplitude begins at 2525.7,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2589,2590,  0,     86,    86 }, // 2355: f53GM102; FX 7 echoes

    // Amplitude begins at 3862.7, peaks 4075.0 at 0.0s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 2591,2592,  0,    573,   573 }, // 2356: f53GM103; FX 8 sci-fi

    // Amplitude begins at 2296.0,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 2593,2594,  0,    326,   326 }, // 2357: f53GM104; Sitar

    // Amplitude begins at    0.8, peaks 2661.2 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 2595,2595,  0,  40000,    53 }, // 2358: f53GM105; Banjo

    // Amplitude begins at  855.6, peaks 1264.8 at 0.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2596,2597,  0,  40000,    26 }, // 2359: f53GM107; Koto

    // Amplitude begins at 1687.4, peaks 1831.8 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2598,2599,  0,  40000,     6 }, // 2360: f53GM108; Kalimba

    // Amplitude begins at  609.7, peaks 3001.6 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2600,2601,  0,  40000,    20 }, // 2361: f53GM109; Bagpipe

    // Amplitude begins at  609.8, peaks 2379.0 at 37.8s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2602,2603,  0,  40000,    20 }, // 2362: f53GM110; Fiddle

    // Amplitude begins at  824.7, peaks  962.2 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 2604,2605,  0,  40000,   106 }, // 2363: f53GM111; Shanai

    // Amplitude begins at 2233.5, peaks 3346.1 at 0.1s,
    // fades to 20% at 3.4s, keyoff fades to 20% in 3.4s.
    { 2606,2606,  0,   3446,  3446 }, // 2364: f53GM112; Tinkle Bell

    // Amplitude begins at  902.1, peaks  927.7 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 2607,2607,  0,  40000,   113 }, // 2365: f53GM113; Agogo Bells

    // Amplitude begins at 3345.3,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 2608,2609,  0,    346,   346 }, // 2366: f53GM116; Taiko Drum

    // Amplitude begins at   14.8, peaks 3238.2 at 0.0s,
    // fades to 20% at 0.8s, keyoff fades to 20% in 0.8s.
    { 2610,2610,  0,    820,   820 }, // 2367: f53GM119; Reverse Cymbal

    // Amplitude begins at 1266.9,
    // fades to 20% at 1.3s, keyoff fades to 20% in 1.3s.
    { 2611,2611,  0,   1293,  1293 }, // 2368: f53GM120; Guitar FretNoise

    // Amplitude begins at 2074.0, peaks 2328.4 at 0.0s,
    // fades to 20% at 1.1s, keyoff fades to 20% in 1.1s.
    { 2612,2613,  0,   1106,  1106 }, // 2369: f53GM121; Breath Noise

    // Amplitude begins at    4.5, peaks 1887.4 at 28.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.3s.
    { 2614,2614,  0,  40000,   273 }, // 2370: f53GM123; Bird Tweet

    // Amplitude begins at  294.5, peaks  776.2 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 2615,2615,  0,    286,   286 }, // 2371: f53GM124; Telephone

    // Amplitude begins at  656.2, peaks 1683.0 at 0.0s,
    // fades to 20% at 3.6s, keyoff fades to 20% in 3.6s.
    { 2616,2617,  0,   3593,  3593 }, // 2372: f53GM126; Applause/Noise

    // Amplitude begins at 2072.6, peaks 2221.3 at 1.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.0s.
    { 2618,2619,  0,  40000,   953 }, // 2373: f53GM127; Gunshot

    // Amplitude begins at  641.1,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 2620,2620,  0,    620,   620 }, // 2374: f54GM81; Lead 2 sawtooth

    // Amplitude begins at 1034.3, peaks 1243.0 at 31.4s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.1s.
    { 2621,2621,  0,  40000,  1133 }, // 2375: f54GM87; f54GM90; Lead 8 brass; Pad 3 polysynth

    // Amplitude begins at 1870.2, peaks 1915.0 at 0.0s,
    // fades to 20% at 0.7s, keyoff fades to 20% in 0.7s.
    { 2622,2622,  0,    653,   653 }, // 2376: b41M0; china2.i

    // Amplitude begins at 2905.8, peaks 2946.9 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.3s.
    { 2623,2623,  0,  40000,   300 }, // 2377: b41M7; china1.i

    // Amplitude begins at 1182.8, peaks 1262.5 at 27.7s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2624,2624,  0,  40000,     6 }, // 2378: b41M9; car2.ins

    // Amplitude begins at 1888.6, peaks 2294.6 at 4.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 2625,2625,  0,  40000,   246 }, // 2379: b41M28; bassharp

    // Amplitude begins at    6.7, peaks 3533.4 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2626,2626,  0,  40000,    46 }, // 2380: b41M33; flute3.i

    // Amplitude begins at 2934.1,
    // fades to 20% at 4.7s, keyoff fades to 20% in 4.7s.
    { 2627,2627,  0,   4746,  4746 }, // 2381: b41M126; b41M127; b41M34; sitar2.i

    // Amplitude begins at 3201.5,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2628,2628,  0,  40000,     0 }, // 2382: b41M36; banjo3.i

    // Amplitude begins at    0.5, peaks 3081.3 at 0.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.7s.
    { 2629,2629,  0,  40000,  1726 }, // 2383: b41M48; b41M50; strings1

    // Amplitude begins at 1468.1, peaks 1675.6 at 0.7s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2630,2630,  0,  40000,     6 }, // 2384: b41M67; cstacc19

    // Amplitude begins at    1.7, peaks 3453.4 at 39.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.6s.
    { 2631,2631,  0,  40000,  1600 }, // 2385: b41M88; tuba2.in

    // Amplitude begins at  828.7, peaks 2122.1 at 21.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2632,2632,  0,  40000,     0 }, // 2386: b41M89; harmonc2

    // Amplitude begins at 1616.4,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 2633,2633,  0,    573,   573 }, // 2387: b41M98; matilda.

    // Amplitude begins at 1165.9, peaks 1188.4 at 0.0s,
    // fades to 20% at 0.5s, keyoff fades to 20% in 0.5s.
    { 2634,2634,  0,    540,   540 }, // 2388: b41M114; italy.in

    // Amplitude begins at 2020.6, peaks 2519.7 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.1s.
    { 2635,2635,  0,  40000,  1086 }, // 2389: b41M123; entbell.

    // Amplitude begins at    0.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 2636,2636, 35,      0,     0 }, // 2390: b41P0; b41P1; b41P10; b41P100; b41P101; b41P102; b41P103; b41P104; b41P105; b41P106; b41P107; b41P108; b41P109; b41P11; b41P110; b41P111; b41P112; b41P113; b41P114; b41P115; b41P116; b41P117; b41P118; b41P119; b41P12; b41P120; b41P121; b41P122; b41P123; b41P124; b41P125; b41P126; b41P127; b41P13; b41P14; b41P15; b41P16; b41P17; b41P18; b41P19; b41P2; b41P20; b41P21; b41P22; b41P23; b41P24; b41P25; b41P26; b41P27; b41P28; b41P29; b41P3; b41P30; b41P31; b41P32; b41P33; b41P34; b41P4; b41P5; b41P51; b41P54; b41P55; b41P58; b41P59; b41P6; b41P7; b41P74; b41P77; b41P78; b41P79; b41P8; b41P80; b41P81; b41P82; b41P83; b41P84; b41P85; b41P86; b41P87; b41P88; b41P89; b41P9; b41P90; b41P91; b41P92; b41P93; b41P94; b41P95; b41P96; b41P97; b41P98; b41P99; blank.in

    // Amplitude begins at 2487.7, peaks 3602.4 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1549,1549, 48,     46,    46 }, // 2391: b41P36; SBBD.ins

    // Amplitude begins at  773.1,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1635,1635, 52,     80,    80 }, // 2392: b41P37; sn1.ins

    // Amplitude begins at  751.5,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1547,1547, 60,     60,    60 }, // 2393: b41P38; SBSN1.in

    // Amplitude begins at  827.6,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1046,1046, 60,    133,   133 }, // 2394: b41P39; snare2.i

    // Amplitude begins at  980.8,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 269,269, 55,     13,    13 }, // 2395: b41P40; b41P50; b41P62; b41P65; tom.ins

    // Amplitude begins at 1056.7,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 269,269, 41,     13,    13 }, // 2396: b41P41; b41P43; b41P61; b41P66; tom.ins

    // Amplitude begins at  366.6,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1637,1637, 60,     33,    33 }, // 2397: b41P42; hatcl2.i

    // Amplitude begins at  284.8, peaks  428.6 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1638,1638, 84,     73,    73 }, // 2398: b41P44; b41P47; b41P69; b41P70; bcymbal.

    // Amplitude begins at  918.1,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 269,269, 48,     13,    13 }, // 2399: b41P45; b41P56; b41P64; tom.ins

    // Amplitude begins at  329.8,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1639,1639, 60,     60,    60 }, // 2400: b41P46; hatop.in

    // Amplitude begins at 1987.4, peaks 2617.1 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1551,1551,  0,     86,    86 }, // 2401: b41P48; b41P53; tom2.ins

    // Amplitude begins at 1248.3,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1552,1552, 49,     40,    40 }, // 2402: b41P49; claves.i

    // Amplitude begins at 1846.7, peaks 2450.7 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1551,1551, 16,    100,   100 }, // 2403: b41P52; tom2.ins

    // Amplitude begins at 2163.9,
    // fades to 20% at 40.0s, keyoff fades to 20% in 2.2s.
    { 1622,1622, 48,  40000,  2220 }, // 2404: b41P57; cowbell.

    // Amplitude begins at  799.7,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1547,1547, 55,     53,    53 }, // 2405: b41P60; SBSN1.in

    // Amplitude begins at  590.3,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 1630,1630, 95,    573,   573 }, // 2406: b41P63; triangle

    // Amplitude begins at 1221.5,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1552,1552,  0,     46,    46 }, // 2407: b41P67; claves.i

    // Amplitude begins at 1155.8,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1552,1552, 17,     40,    40 }, // 2408: b41P68; claves.i

    // Amplitude begins at 2787.9,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2637,2637, 65,    100,   100 }, // 2409: b41P71; undersn.

    // Amplitude begins at 2262.3,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 1640,1640, 20,    200,   200 }, // 2410: b41P72; arabdrum

    // Amplitude begins at 2332.4,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 1640,1640, 22,    200,   200 }, // 2411: b41P73; arabdrum

    // Amplitude begins at 2268.2,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 1640,1640, 24,    193,   193 }, // 2412: b41P75; arabdrum

    // Amplitude begins at  117.4, peaks 2694.8 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2638,2638,  0,     86,    86 }, // 2413: b41P76; heart.in

    // Amplitude begins at  116.2, peaks  806.8 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2107,2107, 36,     66,    66 }, // 2414: b42P28; clap

    // Amplitude begins at   52.0, peaks  738.9 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2639,2639, 48,     60,    60 }, // 2415: b42P29; scratch

    // Amplitude begins at   13.5, peaks  653.4 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2639,2639, 36,     80,    80 }, // 2416: b42P30; scratch

    // Amplitude begins at 2093.6,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 2109,2109, 36,     20,    20 }, // 2417: b42P31; b42P37; b42P86; RimShot; rimshot

    // Amplitude begins at 2602.3,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 2640,2640, 32,     46,    46 }, // 2418: b42P32; hiq

    // Amplitude begins at 3223.3,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2095,2095, 61,     53,    53 }, // 2419: b42P33; b42P77; woodblok

    // Amplitude begins at 2651.7,
    // fades to 20% at 1.3s, keyoff fades to 20% in 1.3s.
    { 2641,2641, 96,   1286,  1286 }, // 2420: b42P34; glock

    // Amplitude begins at 1928.1,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2111,2111, 30,     53,    53 }, // 2421: b42P35; Kick2

    // Amplitude begins at 1278.9, peaks 1873.5 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2642,2642, 35,     73,    73 }, // 2422: b42P36; Kick

    // Amplitude begins at  834.5,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2112,2112, 60,     80,    80 }, // 2423: b42P38; Snare

    // Amplitude begins at  467.9, peaks  706.9 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 2107,2107, 59,     40,    40 }, // 2424: b42P39; Clap

    // Amplitude begins at  824.2,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2112,2112, 44,     80,    80 }, // 2425: b42P40; Snare

    // Amplitude begins at  768.5, peaks  860.9 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2643,2643, 41,    160,   160 }, // 2426: b42P41; Toms

    // Amplitude begins at  840.4,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 2644,2644, 47,     46,    46 }, // 2427: b42P42; b42P44; clsdht47

    // Amplitude begins at  833.5, peaks  923.0 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2643,2643, 44,    120,   120 }, // 2428: b42P43; Toms

    // Amplitude begins at  793.8,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2643,2643, 48,    160,   160 }, // 2429: b42P45; Toms

    // Amplitude begins at    3.3, peaks 1157.2 at 0.0s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 2645,2645, 62,    626,   626 }, // 2430: b42P46; Openhat

    // Amplitude begins at  769.0, peaks  841.8 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2643,2643, 51,    166,   166 }, // 2431: b42P47; Toms

    // Amplitude begins at  896.6,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2643,2643, 54,    153,   153 }, // 2432: b42P48; Toms

    // Amplitude begins at  416.6, peaks  463.6 at 0.0s,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 2646,2646, 40,    386,   386 }, // 2433: b42P49; b42P52; b42P55; b42P57; Crash

    // Amplitude begins at  922.4,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2643,2643, 57,    106,   106 }, // 2434: b42P50; Toms

    // Amplitude begins at  365.2,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2647,2647, 58,    193,   193 }, // 2435: b42P51; b42P53; Ride

    // Amplitude begins at 2156.1, peaks 2540.2 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2648,2648, 97,    186,   186 }, // 2436: b42P54; Tamb

    // Amplitude begins at 1412.7,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2649,2649, 50,     86,    86 }, // 2437: b42P56; Cowbell

    // Amplitude begins at  362.7,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2647,2647, 60,    186,   186 }, // 2438: b42P59; ride

    // Amplitude begins at  998.1, peaks 2597.5 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 2120,2120, 53,     33,    33 }, // 2439: b42P60; mutecong

    // Amplitude begins at 1843.7, peaks 2293.3 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 2650,2650, 46,     46,    46 }, // 2440: b42P61; conga

    // Amplitude begins at 1073.8, peaks 2378.1 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 2120,2120, 57,     33,    33 }, // 2441: b42P62; mutecong

    // Amplitude begins at  568.7, peaks  827.1 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2651,2651, 42,    200,   200 }, // 2442: b42P63; loconga

    // Amplitude begins at  556.9, peaks  829.5 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2651,2651, 37,    200,   200 }, // 2443: b42P64; loconga

    // Amplitude begins at  742.9,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2652,2652, 41,    193,   193 }, // 2444: b42P65; timbale

    // Amplitude begins at  773.6,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2652,2652, 37,    193,   193 }, // 2445: b42P66; timbale

    // Amplitude begins at  398.3,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 2653,2653, 77,     46,    46 }, // 2446: b42P67; agogo

    // Amplitude begins at  401.9,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 2653,2653, 72,     46,    46 }, // 2447: b42P68; agogo

    // Amplitude begins at    2.8, peaks  448.8 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 2654,2654, 70,     46,    46 }, // 2448: b42P69; b42P82; shaker

    // Amplitude begins at    3.2, peaks  426.6 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 2654,2654, 90,     46,    46 }, // 2449: b42P70; shaker

    // Amplitude begins at 2315.6,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2655,2655, 39,    166,   166 }, // 2450: b42P71; hiwhist

    // Amplitude begins at  507.9, peaks 1262.7 at 0.0s,
    // fades to 20% at 1.0s, keyoff fades to 20% in 1.0s.
    { 2656,2656, 36,   1006,  1006 }, // 2451: b42P72; lowhist

    // Amplitude begins at   37.3, peaks  613.3 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 2657,2657, 46,     33,    33 }, // 2452: b42P73; higuiro

    // Amplitude begins at    0.0, peaks  633.9 at 0.1s,
    // fades to 20% at 0.7s, keyoff fades to 20% in 0.7s.
    { 2658,2658, 48,    680,   680 }, // 2453: b42P74; loguiro

    // Amplitude begins at 1569.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 2659,2659, 85,     26,    26 }, // 2454: b42P75; clave

    // Amplitude begins at 3155.0,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2095,2095, 66,     53,    53 }, // 2455: b42P76; woodblok

    // Amplitude begins at   34.3, peaks 1806.5 at 0.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2660,2660, 41,  40000,    33 }, // 2456: b42P78; hicuica

    // Amplitude begins at    1.1, peaks 1526.1 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2661,2661, 41,     93,    93 }, // 2457: b42P79; locuica

    // Amplitude begins at 2665.0, peaks 2770.4 at 0.0s,
    // fades to 20% at 3.4s, keyoff fades to 20% in 3.4s.
    { 2132,2132, 81,   3433,  3433 }, // 2458: b42P80; mutringl

    // Amplitude begins at 1685.7,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 220,220,  0,    313,   313 }, // 2459: b42P84; triangle

    // Amplitude begins at 1642.4,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 2109,2109, 60,      6,     6 }, // 2460: b42P85; rimShot

    // Amplitude begins at 3497.6, peaks 3646.8 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2096,2096, 53,    240,   240 }, // 2461: b42P87; taiko

    // Amplitude begins at 2276.5, peaks 2858.8 at 0.0s,
    // fades to 20% at 1.7s, keyoff fades to 20% in 1.7s.
    { 2641,2641,  0,   1660,  1660 }, // 2462: b42M9; GLOCK

    // Amplitude begins at 1782.0,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 2662,2662,  0,    426,   426 }, // 2463: b42M37; SLAPBAS2

    // Amplitude begins at  402.3, peaks  423.6 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2653,2653,  0,     86,    86 }, // 2464: b42M113; AGOGO

    // Amplitude begins at    0.0, peaks  968.0 at 1.1s,
    // fades to 20% at 3.3s, keyoff fades to 20% in 3.3s.
    { 2663,2663,  0,   3280,  3280 }, // 2465: b42M119; REVRSCYM

    // Amplitude begins at    0.9, peaks 1799.5 at 0.1s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2664,2664,  0,    193,   193 }, // 2466: b42M120; FRETNOIS

    // Amplitude begins at  216.5, peaks  511.7 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2665,2665,  0,     53,    53 }, // 2467: b42M121; BRTHNOIS

    // Amplitude begins at    0.0, peaks  479.0 at 2.3s,
    // fades to 20% at 2.9s, keyoff fades to 20% in 2.9s.
    { 2666,2666,  0,   2946,  2946 }, // 2468: b42M122; SEASHORE

    // Amplitude begins at    5.8, peaks 1234.4 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2667,2667,  0,    113,   113 }, // 2469: b42M123; BIRDS

    // Amplitude begins at 2867.2, peaks 2922.4 at 33.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2668,2668,  0,  40000,    40 }, // 2470: b42M124; TELEPHON

    // Amplitude begins at    0.0, peaks 1494.9 at 23.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.3s.
    { 2669,2669,  0,  40000,   293 }, // 2471: b42M125; HELICOPT

    // Amplitude begins at    0.0, peaks  477.0 at 0.6s,
    // fades to 20% at 2.9s, keyoff fades to 20% in 2.9s.
    { 2670,2670,  0,   2920,  2920 }, // 2472: b42M126; APPLAUSE

    // Amplitude begins at  727.9,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 196,196, 36,     13,    13 }, // 2473: b43P28; clap

    // Amplitude begins at    6.9, peaks 1341.9 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2671,2671, 48,     80,    80 }, // 2474: b43P29; scratch

    // Amplitude begins at    1.7, peaks 1534.5 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2671,2671, 36,     93,    93 }, // 2475: b43P30; scratch

    // Amplitude begins at 1527.3,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 2672,2672, 32,     46,    46 }, // 2476: b43P32; hiq

    // Amplitude begins at 2532.4,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2673,2673, 96,    226,   226 }, // 2477: b43P34; glocken

    // Amplitude begins at 2637.2,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2674,2674, 30,     73,    73 }, // 2478: b43P35; Kick2

    // Amplitude begins at  498.3, peaks  644.9 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 2675,2675, 96,     40,    40 }, // 2479: b43P42; b43P44; clshat96

    // Amplitude begins at    1.7, peaks  558.9 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2676,2676, 60,    186,   186 }, // 2480: b43P46; Opnhat96

    // Amplitude begins at  216.2, peaks 1704.7 at 0.0s,
    // fades to 20% at 0.7s, keyoff fades to 20% in 0.7s.
    { 2677,2677,  0,    680,   680 }, // 2481: b43M0; PIANO1

    // Amplitude begins at 1630.7, peaks 1643.5 at 0.0s,
    // fades to 20% at 2.2s, keyoff fades to 20% in 2.2s.
    { 2678,2678,  0,   2220,  2220 }, // 2482: b43M1; b44P1; PIANO2; PIANO2.I

    // Amplitude begins at 1766.1, peaks 1910.3 at 0.0s,
    // fades to 20% at 0.7s, keyoff fades to 20% in 0.7s.
    { 2679,2679,  0,    680,   680 }, // 2483: b43M2; b44P2; PIANO3; PIANO3.I

    // Amplitude begins at 1934.1, peaks 2136.2 at 0.0s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 2680,2680,  0,    573,   573 }, // 2484: b43M3; HONKYTNK

    // Amplitude begins at  724.6,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 2681,2681,  0,     40,    40 }, // 2485: b43M4; EPIANO1

    // Amplitude begins at 2625.4, peaks 3283.1 at 0.0s,
    // fades to 20% at 1.8s, keyoff fades to 20% in 1.8s.
    { 2682,2682,  0,   1800,  1800 }, // 2486: b43M5; EPIANO2

    // Amplitude begins at 1001.7, peaks 1093.1 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2683,2683,  0,  40000,     6 }, // 2487: b43M6; b44P6; HARPSI; HARPSI.I

    // Amplitude begins at 2282.5,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2684,2684,  0,  40000,     0 }, // 2488: b43M7; CLAV

    // Amplitude begins at 1778.1,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 2685,2685,  0,    586,   586 }, // 2489: b43M8; b44P8; CELESTA; CELESTA.

    // Amplitude begins at 2460.9, peaks 2570.9 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 2673,2673,  0,    313,   313 }, // 2490: b43M9; b44P9; GLOCKEN; GLOCKEN.

    // Amplitude begins at 1053.6, peaks 1651.0 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2686,2686,  0,    206,   206 }, // 2491: b43M10; b44P10; MUSICBOX

    // Amplitude begins at 2625.8, peaks 3101.2 at 0.0s,
    // fades to 20% at 2.0s, keyoff fades to 20% in 2.0s.
    { 2687,2687,  0,   1993,  1993 }, // 2492: b43M11; b44P11; VIBES; VIBES.IN

    // Amplitude begins at 1011.7,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 2688,2688,  0,    260,   260 }, // 2493: b43M12; b44P12; MARIMBA; MARIMBA.

    // Amplitude begins at 1302.9,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2689,2689,  0,    120,   120 }, // 2494: b43M13; XYLOPHON

    // Amplitude begins at 3051.3, peaks 3171.7 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2690,2690,  0,  40000,     0 }, // 2495: b43M14; TUBEBELL

    // Amplitude begins at 1691.6, peaks 1760.4 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 2691,2691,  0,    293,   293 }, // 2496: b43M15; SANTUR

    // Amplitude begins at 1553.5,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2692,2692,  0,  40000,     6 }, // 2497: b43M16; ORGAN1

    // Amplitude begins at 2651.1, peaks 2840.5 at 38.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2693,2693,  0,  40000,     6 }, // 2498: b43M17; b44P17; ORGAN2; ORGAN2.I

    // Amplitude begins at 1783.6,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 2694,2694,  0,     13,    13 }, // 2499: b43M18; ORGAN3

    // Amplitude begins at    0.3, peaks  997.7 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 2695,2695,  0,  40000,   120 }, // 2500: b43M19; b44P19; CHRCHORG

    // Amplitude begins at    4.5, peaks 3370.1 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 2696,2696,  0,  40000,    53 }, // 2501: b43M20; b44P20; REEDORG; REEDORG.

    // Amplitude begins at    0.3, peaks  857.1 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 2697,2697,  0,  40000,    60 }, // 2502: b43M21; ACCORD

    // Amplitude begins at    0.3, peaks 1671.4 at 15.8s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2698,2698,  0,  40000,    20 }, // 2503: b43M22; b44P22; HARMO; HARMO.IN

    // Amplitude begins at    0.3, peaks 1850.4 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2699,2699,  0,  40000,    33 }, // 2504: b43M23; b44P23; BANDNEON

    // Amplitude begins at 1300.5,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2700,2700,  0,  40000,    20 }, // 2505: b43M24; b44P24; NYLONGT; NYLONGT.

    // Amplitude begins at 1538.4, peaks 1613.2 at 0.0s,
    // fades to 20% at 1.1s, keyoff fades to 20% in 1.1s.
    { 2701,2701,  0,   1113,  1113 }, // 2506: b43M25; STEELGT

    // Amplitude begins at 1847.0, peaks 2295.9 at 0.0s,
    // fades to 20% at 1.6s, keyoff fades to 20% in 1.6s.
    { 2702,2702,  0,   1606,  1606 }, // 2507: b43M26; b44P26; JAZZGT; JAZZGT.I

    // Amplitude begins at  685.0,
    // fades to 20% at 1.0s, keyoff fades to 20% in 1.0s.
    { 2703,2703,  0,   1006,  1006 }, // 2508: b43M27; CLEANGT

    // Amplitude begins at  409.1, peaks 1083.3 at 0.0s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 2704,2704,  0,    593,   593 }, // 2509: b43M28; MUTEGT

    // Amplitude begins at 1596.6, peaks 1634.2 at 0.0s,
    // fades to 20% at 4.8s, keyoff fades to 20% in 4.8s.
    { 2705,2705,  0,   4760,  4760 }, // 2510: b43M29; OVERDGT

    // Amplitude begins at 1576.5, peaks 2274.7 at 0.0s,
    // fades to 20% at 2.0s, keyoff fades to 20% in 2.0s.
    { 2706,2706,  0,   2013,  2013 }, // 2511: b43M30; DISTGT

    // Amplitude begins at 1099.1, peaks 1272.7 at 0.0s,
    // fades to 20% at 2.5s, keyoff fades to 20% in 2.5s.
    { 2707,2707,  0,   2486,  2486 }, // 2512: b43M31; b44P31; GTHARMS; GTHARMS.

    // Amplitude begins at  537.2, peaks 2959.3 at 0.0s,
    // fades to 20% at 0.5s, keyoff fades to 20% in 0.5s.
    { 2708,2708,  0,    453,   453 }, // 2513: b43M32; ACOUBASS

    // Amplitude begins at 2518.5, peaks 3496.3 at 0.0s,
    // fades to 20% at 1.9s, keyoff fades to 20% in 1.9s.
    { 2709,2709,  0,   1940,  1940 }, // 2514: b43M33; FINGBASS

    // Amplitude begins at 1086.8, peaks 1473.3 at 0.0s,
    // fades to 20% at 1.1s, keyoff fades to 20% in 1.1s.
    { 2710,2710,  0,   1066,  1066 }, // 2515: b43M34; PICKBASS

    // Amplitude begins at 1475.8, peaks 3173.6 at 0.0s,
    // fades to 20% at 1.4s, keyoff fades to 20% in 1.4s.
    { 2711,2711,  0,   1413,  1413 }, // 2516: b43M35; FRETLESS

    // Amplitude begins at 1974.7,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2712,2712,  0,    240,   240 }, // 2517: b43M36; SLAPBAS1

    // Amplitude begins at 1255.3,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 2713,2713,  0,    573,   573 }, // 2518: b43M37; SLAPBAS2

    // Amplitude begins at 1471.6, peaks 1839.3 at 0.0s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 2714,2714,  0,    600,   600 }, // 2519: b43M38; SYNBASS1

    // Amplitude begins at 1502.8, peaks 3488.1 at 0.0s,
    // fades to 20% at 1.0s, keyoff fades to 20% in 1.0s.
    { 2715,2715,  0,    960,   960 }, // 2520: b43M39; SYNBASS2

    // Amplitude begins at  995.4,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2716,2716,  0,  40000,    20 }, // 2521: b43M40; b44P40; VIOLIN; VIOLIN.I

    // Amplitude begins at    1.2, peaks  621.8 at 26.5s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2717,2717,  0,  40000,    20 }, // 2522: b43M41; VIOLA

    // Amplitude begins at    1.2, peaks  544.7 at 21.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2718,2718,  0,  40000,    26 }, // 2523: b43M42; b44P42; CELLO; CELLO.IN

    // Amplitude begins at    0.0, peaks  854.1 at 0.1s,
    // fades to 20% at 2.0s, keyoff fades to 20% in 2.0s.
    { 2719,2719,  0,   1993,  1993 }, // 2524: b43M43; b44P43; CONTRAB; CONTRAB.

    // Amplitude begins at    4.8, peaks 1344.7 at 21.7s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 2720,2720,  0,  40000,   133 }, // 2525: b43M44; b44P44; TREMSTR; TREMSTR.

    // Amplitude begins at  739.3, peaks 2298.9 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2721,2721,  0,    153,   153 }, // 2526: b43M45; PIZZ

    // Amplitude begins at  841.1, peaks  873.2 at 0.0s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 2722,2722,  0,    573,   573 }, // 2527: b43M46; b44P46; HARP; HARP.INS

    // Amplitude begins at  118.2, peaks 2805.2 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 2723,2723,  0,    260,   260 }, // 2528: b43M47; TIMPANI

    // Amplitude begins at    0.0, peaks 1545.7 at 5.9s,
    // fades to 20% at 5.9s, keyoff fades to 20% in 0.0s.
    { 2724,2724,  0,   5893,    33 }, // 2529: b43M48; STRINGS

    // Amplitude begins at  618.5, peaks  836.7 at 10.7s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 2725,2725,  0,  40000,   193 }, // 2530: b43M49; b44P49; SLOWSTR; SLOWSTR.

    // Amplitude begins at    0.0, peaks  469.7 at 0.1s,
    // fades to 20% at 1.5s, keyoff fades to 20% in 1.5s.
    { 2726,2726,  0,   1520,  1520 }, // 2531: b43M50; b44P50; SYNSTR1; SYNSTR1.

    // Amplitude begins at    0.0, peaks  929.0 at 1.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2727,2727,  0,  40000,    46 }, // 2532: b43M51; SYNSTR2

    // Amplitude begins at    0.2, peaks 1828.3 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2728,2728,  0,  40000,     6 }, // 2533: b43M52; b44P52; CHOIR; CHOIR.IN

    // Amplitude begins at  116.3, peaks 2833.9 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2729,2729,  0,  40000,    46 }, // 2534: b43M53; b44P53; OOHS; OOHS.INS

    // Amplitude begins at    7.2, peaks 2968.0 at 0.0s,
    // fades to 20% at 1.0s, keyoff fades to 20% in 1.0s.
    { 2730,2730,  0,    960,   960 }, // 2535: b43M54; b44P54; SYNVOX; SYNVOX.I

    // Amplitude begins at   15.5, peaks  489.4 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 2731,2731,  0,    280,   280 }, // 2536: b43M55; b44P55; ORCHIT; ORCHIT.I

    // Amplitude begins at   39.4, peaks 1902.7 at 0.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.7s.
    { 2732,2732,  0,  40000,   733 }, // 2537: b43M56; TRUMPET

    // Amplitude begins at    2.2, peaks  984.9 at 8.8s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2733,2733,  0,  40000,    20 }, // 2538: b43M57; TROMBONE

    // Amplitude begins at    2.2, peaks  979.8 at 21.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2734,2734,  0,  40000,    20 }, // 2539: b43M58; TUBA

    // Amplitude begins at  186.2, peaks 1858.9 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2735,2735,  0,  40000,     6 }, // 2540: b43M59; b44P59; MUTETRP; MUTETRP.

    // Amplitude begins at    8.0, peaks 2835.0 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 2736,2736,  0,  40000,    73 }, // 2541: b43M60; FRHORN

    // Amplitude begins at   94.2, peaks 2219.4 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2737,2737,  0,  40000,    20 }, // 2542: b43M61; TCBRASS1

    // Amplitude begins at   42.3, peaks  938.6 at 0.1s,
    // fades to 20% at 1.6s, keyoff fades to 20% in 0.0s.
    { 2738,2738,  0,   1633,     6 }, // 2543: b43M62; SYNBRAS1

    // Amplitude begins at    2.7, peaks 2834.5 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2739,2739,  0,  40000,    20 }, // 2544: b43M63; b44P63; SYNBRAS2

    // Amplitude begins at    7.5, peaks 3895.2 at 7.5s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2740,2740,  0,  40000,    13 }, // 2545: b43M64; b44P64; SOPSAX; SOPSAX.I

    // Amplitude begins at   25.7, peaks  659.8 at 9.7s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2741,2741,  0,  40000,     6 }, // 2546: b43M65; b44P65; ALTOSAX; ALTOSAX.

    // Amplitude begins at    3.8, peaks  760.2 at 0.0s,
    // fades to 20% at 2.2s, keyoff fades to 20% in 2.2s.
    { 2742,2742,  0,   2213,  2213 }, // 2547: b43M66; b44P66; TENSAX; TENSAX.I

    // Amplitude begins at    3.8, peaks 1649.2 at 18.5s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2743,2743,  0,  40000,    13 }, // 2548: b43M67; b44P67; BARISAX; BARISAX.

    // Amplitude begins at    0.4, peaks 2923.1 at 5.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2744,2744,  0,  40000,     0 }, // 2549: b43M68; OBOE

    // Amplitude begins at   65.3, peaks 1434.7 at 37.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2745,2745,  0,  40000,    20 }, // 2550: b43M69; b44P69; ENGLHORN

    // Amplitude begins at  518.4, peaks 1485.6 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2746,2746,  0,  40000,     0 }, // 2551: b43M70; BASSOON

    // Amplitude begins at    0.0, peaks  944.6 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2747,2747,  0,  40000,     6 }, // 2552: b43M71; CLARINET

    // Amplitude begins at  463.3, peaks 3221.1 at 35.7s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2748,2748,  0,  40000,    33 }, // 2553: b43M72; b44P72; PICCOLO; PICCOLO.

    // Amplitude begins at    7.9, peaks 3359.0 at 30.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2749,2749,  0,  40000,     6 }, // 2554: b43M73; FLUTE1

    // Amplitude begins at    7.9, peaks 2761.2 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2750,2750,  0,  40000,    13 }, // 2555: b43M74; b44P74; RECORDER

    // Amplitude begins at    5.2, peaks 3034.3 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2751,2751,  0,  40000,    20 }, // 2556: b43M75; b44P75; PANFLUTE

    // Amplitude begins at   50.9, peaks 1163.5 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2752,2752,  0,  40000,    40 }, // 2557: b43M76; BOTTLEB

    // Amplitude begins at  594.4, peaks 2025.7 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2753,2753,  0,  40000,    26 }, // 2558: b43M77; SHAKU

    // Amplitude begins at  557.4, peaks 4184.9 at 15.8s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2754,2754,  0,  40000,    20 }, // 2559: b43M78; WHISTLE

    // Amplitude begins at    0.6, peaks 2855.7 at 38.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2755,2755,  0,  40000,    20 }, // 2560: b43M79; OCARINA

    // Amplitude begins at  291.8, peaks 1538.4 at 10.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2756,2756,  0,  40000,     0 }, // 2561: b43M80; SQUARWAV

    // Amplitude begins at 1537.4, peaks 2069.1 at 0.0s,
    // fades to 20% at 1.0s, keyoff fades to 20% in 0.1s.
    { 2757,2757,  0,    960,    60 }, // 2562: b43M81; SAWWAV

    // Amplitude begins at    0.0, peaks  887.4 at 1.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2758,2758,  0,  40000,    20 }, // 2563: b43M82; b44P82; SYNCALLI

    // Amplitude begins at  438.5, peaks 2779.9 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2759,2759,  0,  40000,     0 }, // 2564: b43M83; b44P83; CHIFLEAD

    // Amplitude begins at 1591.2,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2760,2760,  0,     60,    60 }, // 2565: b43M84; CHARANG

    // Amplitude begins at    7.1, peaks 2184.3 at 4.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2761,2761,  0,  40000,     6 }, // 2566: b43M85; b44P85; SOLOVOX; SOLOVOX.

    // Amplitude begins at  341.9, peaks  669.0 at 2.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2762,2762,  0,  40000,    33 }, // 2567: b43M86; FIFTHSAW

    // Amplitude begins at 2158.8, peaks 2366.4 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2763,2763,  0,  40000,     6 }, // 2568: b43M87; BASSLEAD

    // Amplitude begins at  717.9,
    // fades to 20% at 1.0s, keyoff fades to 20% in 1.0s.
    { 2764,2764,  0,    960,   960 }, // 2569: b43M88; b44P88; FANTASIA

    // Amplitude begins at    0.0, peaks  906.5 at 0.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 2765,2765,  0,  40000,   233 }, // 2570: b43M89; WARMPAD

    // Amplitude begins at  620.0, peaks 3085.0 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 2766,2766,  0,  40000,    86 }, // 2571: b43M90; POLYSYN

    // Amplitude begins at  139.8, peaks 2881.6 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2767,2767,  0,  40000,    26 }, // 2572: b43M91; SPACEVOX

    // Amplitude begins at    0.0, peaks 3282.6 at 0.2s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2768,2768,  0,  40000,    20 }, // 2573: b43M92; BOWEDGLS

    // Amplitude begins at   78.8, peaks 1917.4 at 0.6s,
    // fades to 20% at 2.2s, keyoff fades to 20% in 2.2s.
    { 2769,2769,  0,   2233,  2233 }, // 2574: b43M93; METALPAD

    // Amplitude begins at    0.0, peaks  959.8 at 0.1s,
    // fades to 20% at 2.3s, keyoff fades to 20% in 0.1s.
    { 2770,2770,  0,   2280,    53 }, // 2575: b43M94; HALOPAD

    // Amplitude begins at    0.0, peaks 1245.2 at 1.2s,
    // fades to 20% at 4.3s, keyoff fades to 20% in 0.1s.
    { 2771,2771,  0,   4320,    80 }, // 2576: b43M95; SWEEPPAD

    // Amplitude begins at  265.3, peaks  922.5 at 0.0s,
    // fades to 20% at 2.8s, keyoff fades to 20% in 2.8s.
    { 2772,2772,  0,   2773,  2773 }, // 2577: b43M96; b44P96; ICERAIN; ICERAIN.

    // Amplitude begins at    0.0, peaks  658.6 at 5.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.3s.
    { 2773,2773,  0,  40000,   260 }, // 2578: b43M97; SOUNDTRK

    // Amplitude begins at 2853.5, peaks 2880.5 at 0.0s,
    // fades to 20% at 1.0s, keyoff fades to 20% in 1.0s.
    { 2774,2774,  0,   1013,  1013 }, // 2579: b43M98; CRYSTAL

    // Amplitude begins at  909.2, peaks  919.8 at 0.0s,
    // fades to 20% at 0.7s, keyoff fades to 20% in 0.7s.
    { 2775,2775,  0,    660,   660 }, // 2580: b43M99; ATMOSPH

    // Amplitude begins at  420.2, peaks  500.7 at 0.0s,
    // fades to 20% at 2.0s, keyoff fades to 20% in 2.0s.
    { 2776,2776,  0,   1953,  1953 }, // 2581: b43M100; BRIGHT

    // Amplitude begins at    0.0, peaks  909.2 at 0.3s,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.1s.
    { 2777,2777,  0,    426,    53 }, // 2582: b43M101; b44P101; GOBLIN; GOBLIN.I

    // Amplitude begins at 1222.4, peaks 1745.4 at 0.0s,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.9s.
    { 2778,2778,  0,    906,   906 }, // 2583: b43M102; ECHODROP

    // Amplitude begins at   78.8, peaks  772.6 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 2779,2779,  0,  40000,   193 }, // 2584: b43M103; STARTHEM

    // Amplitude begins at  732.2, peaks 1512.2 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 2780,2780,  0,  40000,    73 }, // 2585: b43M104; b44P104; SITAR; SITAR.IN

    // Amplitude begins at 1635.1,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 2781,2781,  0,    253,   253 }, // 2586: b43M105; BANJO

    // Amplitude begins at 1364.4, peaks 2221.1 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.8s.
    { 2782,2782,  0,  40000,  1786 }, // 2587: b43M106; SHAMISEN

    // Amplitude begins at 1643.5,
    // fades to 20% at 1.0s, keyoff fades to 20% in 1.0s.
    { 2783,2783,  0,   1026,  1026 }, // 2588: b43M107; b44P107; KOTO; KOTO.INS

    // Amplitude begins at 1570.1,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2784,2784,  0,    100,   100 }, // 2589: b43M108; b44P108; KALIMBA; KALIMBA.

    // Amplitude begins at    2.3, peaks 1053.1 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2785,2785,  0,  40000,    20 }, // 2590: b43M109; b44P109; BAGPIPE; BAGPIPE.

    // Amplitude begins at  852.9, peaks 1422.8 at 12.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2786,2786,  0,  40000,    33 }, // 2591: b43M110; b44P110; FIDDLE; FIDDLE.I

    // Amplitude begins at 1319.6, peaks 2118.1 at 8.8s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2787,2787,  0,  40000,    20 }, // 2592: b43M111; SHANNAI

    // Amplitude begins at 3139.3, peaks 3229.8 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 2788,2788,  0,  40000,   180 }, // 2593: b43M112; b44P112; TINKLBEL

    // Amplitude begins at  695.2, peaks  738.6 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 209,209,  0,     86,    86 }, // 2594: b43M113; AGOGO

    // Amplitude begins at 1051.3, peaks 1597.9 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2789,2789,  0,    226,   226 }, // 2595: b43M114; STEELDRM

    // Amplitude begins at 2679.7,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 216,216,  0,     80,    80 }, // 2596: b43M115; b44P115; WOODBLOK

    // Amplitude begins at  979.0,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 221,221,  0,     60,    60 }, // 2597: b43M116; b44P116; TAIKO; TAIKO.IN

    // Amplitude begins at 1226.5, peaks 2082.7 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 2790,2790,  0,  40000,   246 }, // 2598: b43M117; MELOTOM

    // Amplitude begins at 1456.1,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2791,2791,  0,    173,   173 }, // 2599: b43M118; SYNDRUM

    // Amplitude begins at    0.0, peaks 1011.0 at 1.2s,
    // fades to 20% at 3.3s, keyoff fades to 20% in 3.3s.
    { 2792,2792,  0,   3280,  3280 }, // 2600: b43M119; REVRSCYM

    // Amplitude begins at    0.2, peaks  650.0 at 0.1s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2793,2793,  0,    226,   226 }, // 2601: b43M120; b44P120; FRETNOIS

    // Amplitude begins at    1.1, peaks  454.5 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2794,2794,  0,    193,   193 }, // 2602: b43M121; BRTHNOIS

    // Amplitude begins at    0.0, peaks  468.4 at 2.3s,
    // fades to 20% at 2.9s, keyoff fades to 20% in 2.9s.
    { 2795,2795,  0,   2933,  2933 }, // 2603: b43M122; SEASHORE

    // Amplitude begins at  777.4, peaks 1262.9 at 0.0s,
    // fades to 20% at 2.4s, keyoff fades to 20% in 2.4s.
    { 2796,2796,  0,   2446,  2446 }, // 2604: b43M123; b44P123; BIRDS; BIRDS.IN

    // Amplitude begins at 2903.2, peaks 2908.9 at 30.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2797,2797,  0,  40000,    40 }, // 2605: b43M124; b44P124; TELEPHON

    // Amplitude begins at  603.7, peaks 1599.4 at 35.7s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 2798,2798,  0,  40000,   200 }, // 2606: b43M125; HELICOPT

    // Amplitude begins at    0.0, peaks  486.0 at 0.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 2799,2799,  0,  40000,   153 }, // 2607: b43M126; APPLAUSE

    // Amplitude begins at  600.5, peaks  856.6 at 0.0s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 2800,2800,  0,    633,   633 }, // 2608: b43M127; GUNSHOT

    // Amplitude begins at   47.2, peaks 2636.7 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2801,2801, 35,     53,    53 }, // 2609: b44M36; Kick.ins

    // Amplitude begins at 2528.8,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2802,2802, 77,     80,    80 }, // 2610: b44M67; agogo.in

    // Amplitude begins at 2543.0,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2802,2802, 72,     86,    86 }, // 2611: b44M68; agogo.in

    // Amplitude begins at 2422.7, peaks 2672.9 at 0.0s,
    // fades to 20% at 1.5s, keyoff fades to 20% in 1.5s.
    { 2803,2803,  0,   1500,  1500 }, // 2612: b44P0; PIANO1.I

    // Amplitude begins at 1658.5,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2804,2804,  0,  40000,     6 }, // 2613: b44P3; TCSAWWAV

    // Amplitude begins at 2883.5, peaks 2955.1 at 0.0s,
    // fades to 20% at 1.9s, keyoff fades to 20% in 1.9s.
    { 2805,2805,  0,   1940,  1940 }, // 2614: b44P4; EPIANO1.

    // Amplitude begins at 1638.3, peaks 1838.6 at 0.0s,
    // fades to 20% at 1.1s, keyoff fades to 20% in 1.1s.
    { 2806,2806,  0,   1100,  1100 }, // 2615: b44P5; EPIANO2.

    // Amplitude begins at  912.9, peaks 1087.7 at 0.0s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 2807,2807,  0,    593,   593 }, // 2616: b44P7; TCCLAV.I

    // Amplitude begins at 1930.8,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2808,2808,  0,    126,   126 }, // 2617: b44P13; XYLOPHON

    // Amplitude begins at 1710.6, peaks 1727.3 at 0.0s,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    { 2809,2809,  0,   1220,  1220 }, // 2618: b44P14; TCBELL.I

    // Amplitude begins at 1759.0, peaks 2010.9 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2810,2810,  0,    206,   206 }, // 2619: b44P15; SANTUR.I

    // Amplitude begins at  935.8, peaks 1088.6 at 5.4s,
    // fades to 20% at 5.4s, keyoff fades to 20% in 0.0s.
    { 2811,2811,  0,   5400,    13 }, // 2620: b44P16; ORGAN1.I

    // Amplitude begins at 2726.5,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2812,2812,  0,  40000,     0 }, // 2621: b44P18; ORGAN3.I

    // Amplitude begins at    0.0, peaks  504.5 at 0.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2813,2813,  0,  40000,    20 }, // 2622: b44P21; ACCORD.I

    // Amplitude begins at 1652.5,
    // fades to 20% at 1.1s, keyoff fades to 20% in 1.1s.
    { 2814,2814,  0,   1133,  1133 }, // 2623: b44P25; STEELGT.

    // Amplitude begins at 1928.7,
    // fades to 20% at 1.1s, keyoff fades to 20% in 1.1s.
    { 2815,2815,  0,   1093,  1093 }, // 2624: b44P27; CLEANGT.

    // Amplitude begins at 3015.2,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 2816,2816,  0,    600,   600 }, // 2625: b44P28; MUTEGT.I

    // Amplitude begins at 1487.1,
    // fades to 20% at 4.9s, keyoff fades to 20% in 4.9s.
    { 2817,2817,  0,   4913,  4913 }, // 2626: b44P29; TCOVRDGT

    // Amplitude begins at 1876.0,
    // fades to 20% at 0.7s, keyoff fades to 20% in 0.7s.
    { 2818,2818,  0,    700,   700 }, // 2627: b44P30; TCDISTG2

    // Amplitude begins at 2651.0,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2819,2819,  0,  40000,    40 }, // 2628: b44P32; ACOUBASS

    // Amplitude begins at  847.6, peaks  895.8 at 0.0s,
    // fades to 20% at 2.2s, keyoff fades to 20% in 2.2s.
    { 2820,2820,  0,   2246,  2246 }, // 2629: b44P33; FINGBASS

    // Amplitude begins at 1568.3, peaks 1766.6 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 2821,2821,  0,  40000,   146 }, // 2630: b44P34; PICKBASS

    // Amplitude begins at 1238.9,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    { 2822,2822,  0,   1153,  1153 }, // 2631: b44P35; FRETLESS

    // Amplitude begins at 2660.6, peaks 2709.2 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 2823,2823,  0,    313,   313 }, // 2632: b44P36; SLAPBAS1

    // Amplitude begins at 3363.2,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 2824,2824,  0,    560,   560 }, // 2633: b44P37; SLAPBAS2

    // Amplitude begins at  805.7, peaks  811.3 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2825,2825,  0,     60,    60 }, // 2634: b44P38; SYNBASS1

    // Amplitude begins at 1669.8, peaks 2892.1 at 0.0s,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 2826,2826,  0,    386,   386 }, // 2635: b44P39; SYNBASS2

    // Amplitude begins at  604.1,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 2827,2827,  0,  40000,   193 }, // 2636: b44P41; VIOLA.IN

    // Amplitude begins at 1299.3, peaks 1783.1 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2828,2828,  0,    120,   120 }, // 2637: b44P45; PIZZ.INS

    // Amplitude begins at 2620.2, peaks 2845.2 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 2829,2829,  0,    266,   266 }, // 2638: b44P47; TIMPANI.

    // Amplitude begins at 1024.5, peaks 1620.1 at 38.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 2830,2830,  0,  40000,   213 }, // 2639: b44P48; STRINGS.

    // Amplitude begins at    0.0, peaks  809.1 at 0.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.3s.
    { 2831,2831,  0,  40000,   280 }, // 2640: b44P51; SYNSTR2.

    // Amplitude begins at   40.6, peaks 2417.8 at 0.5s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.0s.
    { 2832,2832,  0,  40000,   966 }, // 2641: b44P56; TRUMPET.

    // Amplitude begins at    2.2, peaks  891.7 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2833,2833,  0,  40000,    20 }, // 2642: b44P57; TROMBONE

    // Amplitude begins at    2.3, peaks  881.3 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 2834,2834,  0,  40000,    53 }, // 2643: b44P58; TUBA.INS

    // Amplitude begins at    9.5, peaks 4176.4 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 2835,2835,  0,  40000,    73 }, // 2644: b44P60; FRHORN2.

    // Amplitude begins at  120.5, peaks 3179.2 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2836,2836,  0,  40000,    20 }, // 2645: b44P61; TCBRASS1

    // Amplitude begins at   42.6, peaks  891.1 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2837,2837,  0,  40000,    20 }, // 2646: b44P62; SYNBRAS1

    // Amplitude begins at    0.4, peaks 2900.3 at 0.7s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2838,2838,  0,  40000,    20 }, // 2647: b44P68; OBOE.INS

    // Amplitude begins at  953.5, peaks 1943.6 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2839,2839,  0,  40000,     6 }, // 2648: b44P70; BASSOON.

    // Amplitude begins at    0.0, peaks  821.7 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2840,2840,  0,  40000,    33 }, // 2649: b44P71; CLARINET

    // Amplitude begins at    7.3, peaks 3264.1 at 33.7s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2841,2841,  0,  40000,     6 }, // 2650: b44P73; FLUTE1.I

    // Amplitude begins at  876.5, peaks 3201.9 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 2842,2842,  0,  40000,   126 }, // 2651: b44P76; SHAKU.IN

    // Amplitude begins at    0.5, peaks 1094.5 at 0.1s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2843,2843,  0,    206,   206 }, // 2652: b44P77; TCCHIFF.

    // Amplitude begins at    7.6, peaks 3302.4 at 28.5s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 2844,2844,  0,  40000,    73 }, // 2653: b44P79; OCARINA.

    // Amplitude begins at  442.9, peaks 1094.0 at 0.0s,
    // fades to 20% at 3.3s, keyoff fades to 20% in 3.3s.
    { 2845,2845,  0,   3286,  3286 }, // 2654: b44P80; SQUARWAV

    // Amplitude begins at 1651.6,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 2846,2846,  0,     13,    13 }, // 2655: b44P81; SAWWAV.I

    // Amplitude begins at 1611.2,
    // fades to 20% at 4.5s, keyoff fades to 20% in 4.5s.
    { 2847,2847,  0,   4493,  4493 }, // 2656: b44P84; CHARANG.

    // Amplitude begins at  284.0, peaks  559.6 at 3.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2848,2848,  0,  40000,    33 }, // 2657: b44P86; FIFTHSAW

    // Amplitude begins at 1672.4, peaks 1817.8 at 0.1s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2849,2849,  0,  40000,     0 }, // 2658: b44P87; BASSLEAD

    // Amplitude begins at 1204.8, peaks 2676.8 at 0.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 1.9s.
    { 2850,2850,  0,  40000,  1893 }, // 2659: b44P89; WARMPAD.

    // Amplitude begins at 1515.8, peaks 1609.8 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 2851,2851,  0,  40000,   226 }, // 2660: b44P90; POLYSYN.

    // Amplitude begins at  715.6, peaks 1787.3 at 0.6s,
    // fades to 20% at 2.3s, keyoff fades to 20% in 0.1s.
    { 2852,2852,  0,   2273,    80 }, // 2661: b44P91; SPACEVOX

    // Amplitude begins at    0.0, peaks 1704.4 at 0.6s,
    // fades to 20% at 2.7s, keyoff fades to 20% in 2.7s.
    { 2853,2853,  0,   2686,  2686 }, // 2662: b44P92; BOWEDGLS

    // Amplitude begins at    4.9, peaks 2001.7 at 0.2s,
    // fades to 20% at 2.3s, keyoff fades to 20% in 0.0s.
    { 2854,2854,  0,   2340,    40 }, // 2663: b44P93; METALPAD

    // Amplitude begins at    0.0, peaks  874.9 at 0.3s,
    // fades to 20% at 4.7s, keyoff fades to 20% in 4.7s.
    { 2855,2855,  0,   4706,  4706 }, // 2664: b44P94; HALOPAD.

    // Amplitude begins at    0.0, peaks 4132.0 at 0.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 2856,2856,  0,  40000,   193 }, // 2665: b44P95; SWEEPPAD

    // Amplitude begins at    0.0, peaks  660.6 at 37.8s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.3s.
    { 2857,2857,  0,  40000,   260 }, // 2666: b44P97; SOUNDTRK

    // Amplitude begins at 2528.9, peaks 2693.0 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2802,2802,  0,    166,   166 }, // 2667: b44P98; CRYSTAL.

    // Amplitude begins at  459.1, peaks  569.9 at 3.9s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.5s.
    { 2858,2858,  0,  40000,   513 }, // 2668: b44P99; TCSYNTH1

    // Amplitude begins at  430.4,
    // fades to 20% at 2.1s, keyoff fades to 20% in 2.1s.
    { 2859,2859,  0,   2053,  2053 }, // 2669: b44P100; BRIGHT.I

    // Amplitude begins at  781.5, peaks 1714.9 at 32.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.4s.
    { 2860,2860,  0,  40000,   386 }, // 2670: b44P102; ECHODROP

    // Amplitude begins at    3.9, peaks 2012.7 at 0.3s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.1s.
    { 2861,2861,  0,  40000,   106 }, // 2671: b44P103; STARTHEM

    // Amplitude begins at 2759.2,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2862,2862,  0,    153,   153 }, // 2672: b44P105; BANJO.IN

    // Amplitude begins at 1178.7,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2863,2863,  0,    100,   100 }, // 2673: b44P106; SHAMISEN

    // Amplitude begins at 1675.8, peaks 2613.8 at 0.0s,
    // fades to 20% at 1.4s, keyoff fades to 20% in 1.4s.
    { 2864,2864,  0,   1433,  1433 }, // 2674: b44P111; TCPAD1.I

    // Amplitude begins at    0.0, peaks 2935.1 at 2.4s,
    // fades to 20% at 3.4s, keyoff fades to 20% in 0.0s.
    { 2865,2865,  0,   3400,     6 }, // 2675: b44P113; TCLOWPAD

    // Amplitude begins at    0.2, peaks 1038.6 at 1.0s,
    // fades to 20% at 5.6s, keyoff fades to 20% in 0.0s.
    { 2866,2866,  0,   5633,    13 }, // 2676: b44P114; TCPAD4.I

    // Amplitude begins at    0.0, peaks 1443.6 at 0.3s,
    // fades to 20% at 5.2s, keyoff fades to 20% in 5.2s.
    { 2867,2867,  0,   5166,  5166 }, // 2677: b44P117; TCPAD7.I

    // Amplitude begins at    0.0, peaks 2206.6 at 0.7s,
    // fades to 20% at 1.0s, keyoff fades to 20% in 0.0s.
    { 2868,2868,  0,    993,    26 }, // 2678: b44P118; TCPAD8.I

    // Amplitude begins at  600.7, peaks 2321.0 at 0.1s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 2869,2869,  0,    146,   146 }, // 2679: b44P119; TCSFX1.I

    // Amplitude begins at 1226.2, peaks 1245.2 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 2870,2870,  0,  40000,     0 }, // 2680: b44P121; BRTHNOIS

    // Amplitude begins at    3.2, peaks 1202.3 at 0.1s,
    // fades to 20% at 4.9s, keyoff fades to 20% in 4.9s.
    { 2871,2871,  0,   4946,  4946 }, // 2681: b44P122; TCPAD2.I

    // Amplitude begins at    0.0, peaks 1005.5 at 0.6s,
    // fades to 20% at 5.2s, keyoff fades to 20% in 5.2s.
    { 2872,2872,  0,   5173,  5173 }, // 2682: b44P125; TCPAD5.I

    // Amplitude begins at    0.0, peaks  837.7 at 0.6s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.2s.
    { 2873,2873,  0,  40000,   240 }, // 2683: b44P126; TCPAD6.I

    // Amplitude begins at   52.0, peaks  924.7 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 191,191,  0,    333,   333 }, // 2684: b44P127; WIERD2.I

    // Amplitude begins at 2509.7,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 128,128, 52,     20,    20 }, // 2685: b45P37; aps037

    // Amplitude begins at  615.9,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 129,129, 48,     80,    80 }, // 2686: b45P38; aps038

    // Amplitude begins at 2088.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 130,130, 58,     40,    40 }, // 2687: b45P39; aps039

    // Amplitude begins at  606.7,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 129,129, 60,     80,    80 }, // 2688: b45P40; aps040

    // Amplitude begins at  256.7,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 132,132, 43,     26,    26 }, // 2689: b45P42; aps042

    // Amplitude begins at   43.8, peaks  493.1 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 133,133, 43,     26,    26 }, // 2690: b45P44; aps044

    // Amplitude begins at  319.5,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 134,134, 43,    253,   253 }, // 2691: b45P46; aps046

    // Amplitude begins at  380.1, peaks  387.8 at 0.0s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 135,135, 72,    573,   573 }, // 2692: b45P49; aps049

    // Amplitude begins at  473.8,
    // fades to 20% at 0.5s, keyoff fades to 20% in 0.5s.
    { 136,136, 76,    506,   506 }, // 2693: b45P51; aps051

    // Amplitude begins at  931.9,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 138,138, 36,    386,   386 }, // 2694: b45P53; aps053

    // Amplitude begins at 1159.1, peaks 1433.1 at 0.0s,
    // fades to 20% at 0.5s, keyoff fades to 20% in 0.5s.
    { 139,139, 76,    466,   466 }, // 2695: b45P54; aps054

    // Amplitude begins at  367.4, peaks  378.6 at 0.0s,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 140,140, 84,    420,   420 }, // 2696: b45P55; aps055

    // Amplitude begins at 1444.1,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 141,141, 83,     53,    53 }, // 2697: b45P56; aps056

    // Amplitude begins at  388.5,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 135,135, 84,    400,   400 }, // 2698: b45P57; aps057

    // Amplitude begins at  691.5,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 142,142, 24,     13,    13 }, // 2699: b45P58; aps058

    // Amplitude begins at  468.1,
    // fades to 20% at 0.5s, keyoff fades to 20% in 0.5s.
    { 136,136, 77,    513,   513 }, // 2700: b45P59; aps051

    // Amplitude begins at 1809.4,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 143,143, 60,     40,    40 }, // 2701: b45P60; aps060

    // Amplitude begins at 1360.3,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 144,144, 65,     40,    40 }, // 2702: b45P61; aps061

    // Amplitude begins at 2805.8,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 145,145, 59,     13,    13 }, // 2703: b45P62; aps062

    // Amplitude begins at 1551.8,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 146,146, 51,     40,    40 }, // 2704: b45P63; aps063

    // Amplitude begins at 2245.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 147,147, 45,     33,    33 }, // 2705: b45P64; aps064

    // Amplitude begins at  888.0,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 149,149, 60,    140,   140 }, // 2706: b45P66; aps066

    // Amplitude begins at  780.2,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 150,150, 58,    153,   153 }, // 2707: b45P67; aps067

    // Amplitude begins at 1327.3,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 151,151, 53,    100,   100 }, // 2708: b45P68; aps068

    // Amplitude begins at    0.9, peaks  358.3 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 152,152, 64,     86,    86 }, // 2709: b45P69; aps069

    // Amplitude begins at  304.0, peaks  315.3 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 153,153, 71,     13,    13 }, // 2710: b45P70; aps070

    // Amplitude begins at   37.5, peaks  789.5 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 154,154, 61,    326,   326 }, // 2711: b45P71; aps071

    // Amplitude begins at   34.3, peaks  858.7 at 0.0s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 155,155, 61,    606,   606 }, // 2712: b45P72; aps072

    // Amplitude begins at    1.2, peaks  466.5 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 156,156, 48,     80,    80 }, // 2713: b45P73; aps073

    // Amplitude begins at    0.0, peaks  477.3 at 0.1s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 157,157, 48,    213,   213 }, // 2714: b45P74; aps074

    // Amplitude begins at 2076.2,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 158,158, 69,     20,    20 }, // 2715: b45P75; aps075

    // Amplitude begins at 2255.7,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 159,159, 68,     20,    20 }, // 2716: b45P76; aps076

    // Amplitude begins at 2214.7,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 160,160, 63,     33,    33 }, // 2717: b45P77; aps077

    // Amplitude begins at   15.6, peaks 2724.8 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 161,161, 74,     93,    93 }, // 2718: b45P78; aps078

    // Amplitude begins at 1240.9, peaks 2880.7 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 162,162, 60,    346,   346 }, // 2719: b45P79; aps079

    // Amplitude begins at 1090.9,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 163,163, 80,     60,    60 }, // 2720: b45P80; aps080

    // Amplitude begins at 1193.8,
    // fades to 20% at 0.9s, keyoff fades to 20% in 0.9s.
    { 164,164, 64,    880,   880 }, // 2721: b45P81; aps081

    // Amplitude begins at    3.7, peaks  333.4 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 165,165, 69,     40,    40 }, // 2722: b45P82; aps082

    // Amplitude begins at    0.0, peaks  937.8 at 0.1s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 166,166, 73,    293,   293 }, // 2723: b45P83; aps083

    // Amplitude begins at  880.3,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 167,167, 75,    166,   166 }, // 2724: b45P84; aps084

    // Amplitude begins at 1135.9,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 168,168, 68,     20,    20 }, // 2725: b45P85; aps085

    // Amplitude begins at 2185.2,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 169,169, 48,     26,    26 }, // 2726: b45P86; aps086

    // Amplitude begins at   29.2, peaks 1488.6 at 0.0s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 887,888, 35,    606,   606 }, // 2727: b46P72; gps072

    // Amplitude begins at 1922.5,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 859,860, 35,     53,    53 }, // 2728: b46P56; gps056

    // Amplitude begins at 1795.9, peaks 3306.4 at 14.5s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 633,2874,  0,  40000,     6 }, // 2729: b46M18; b47M18; gm018

    // Amplitude begins at   82.9, peaks 3179.7 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 705,2875,  0,  40000,    26 }, // 2730: b46M58; b47M58; gm058

    // Amplitude begins at 3362.0, peaks 3656.3 at 0.1s,
    // fades to 20% at 2.7s, keyoff fades to 20% in 2.7s.
    { 604,2876,  0,   2726,  2726 }, // 2731: b46M3; b47M3; gm003

    // Amplitude begins at 2051.9, peaks 2655.3 at 0.1s,
    // fades to 20% at 0.8s, keyoff fades to 20% in 0.8s.
    { 612,2877,  0,    773,   773 }, // 2732: b46M7; b47M7; gm007

    // Amplitude begins at 1714.4,
    // fades to 20% at 1.4s, keyoff fades to 20% in 1.4s.
    { 619,2878,  0,   1433,  1433 }, // 2733: b46M11; b47M11; gm011

    // Amplitude begins at 1659.0, peaks 1766.4 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 828,2879,  0,    153,   153 }, // 2734: b46M124; b47M124; gm124

    // Amplitude begins at    0.0, peaks 1482.8 at 1.6s,
    // fades to 20% at 1.6s, keyoff fades to 20% in 1.6s.
    { 830,2880,  0,   1580,  1580 }, // 2735: b46M125; b47M125; gm125

    // Amplitude begins at 2571.0,
    // fades to 20% at 1.2s, keyoff fades to 20% in 1.2s.
    { 905,906, 35,   1233,  1233 }, // 2736: b46P83; gps083

    // Amplitude begins at  836.8, peaks 1273.7 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 841,2881, 35,     53,    53 }, // 2737: b46P36; b46P43; b46P45; b46P47; b46P48; b46P50; gps036; gps043; gps045; gps047; gps048; gps050

    // Amplitude begins at 1640.1,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 849,850, 35,    360,   360 }, // 2738: b46P51; gps051

    // Amplitude begins at 1302.7, peaks 1321.7 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 853,854, 35,    326,   326 }, // 2739: b46P53; gps053

    // Amplitude begins at 1631.1,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 1553,1554, 35,    280,   280 }, // 2740: b46P59; gps059

    // Amplitude begins at 1246.7, peaks 1327.3 at 0.0s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 907,908, 35,    286,   286 }, // 2741: b46P84; gps084

    // Amplitude begins at 5032.2,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 892,893, 35,     60,    60 }, // 2742: b46P75; gps075

    // Amplitude begins at 5131.5,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 159,894, 35,     60,    60 }, // 2743: b46P76; b46P77; gps076; gps077

    // Amplitude begins at 2516.1,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 909,910, 35,     60,    60 }, // 2744: b46P85; gps085

    // Amplitude begins at    1.5, peaks  561.1 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 881,882, 35,    100,   100 }, // 2745: b46P69; gps069

    // Amplitude begins at    1.5, peaks  531.5 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 903,904, 35,     46,    46 }, // 2746: b46P82; gps082

    // Amplitude begins at 1770.2,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 145,869, 35,     20,    20 }, // 2747: b46P62; gps062

    // Amplitude begins at 2797.1,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 911,912, 35,     53,    53 }, // 2748: b46P86; gps086

    // Amplitude begins at 3556.4,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 870,871, 35,     40,    40 }, // 2749: b46P63; gps063

    // Amplitude begins at 5011.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 913,914, 35,     13,    13 }, // 2750: b46P87; gps087

    // Amplitude begins at 2272.7, peaks 2433.5 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 841,842, 35,     53,    53 }, // 2751: b46P41; gps041

    // Amplitude begins at 3216.9,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 835,836, 35,     80,    80 }, // 2752: b46P37; gps037

    // Amplitude begins at 1118.0,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 129,840, 35,     66,    66 }, // 2753: b46P40; gps040

    // Amplitude begins at  608.3,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 132,843, 35,     26,    26 }, // 2754: b46P42; gps042

    // Amplitude begins at   20.8, peaks  930.8 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 844,845, 35,     40,    40 }, // 2755: b46P44; gps044

    // Amplitude begins at 1012.9, peaks 1537.2 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 129,837, 35,     66,    66 }, // 2756: b46P38; gps038

    // Amplitude begins at  630.9,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 134,846, 35,    193,   193 }, // 2757: b46P46; gps046

    // Amplitude begins at 1289.3, peaks 2764.3 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 855,856, 35,    126,   126 }, // 2758: b46P54; gps054

    // Amplitude begins at 1742.5, peaks 1872.5 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 857,858, 35,  40000,     0 }, // 2759: b46P55; gps055

    // Amplitude begins at 1786.6, peaks 2084.4 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 865,866, 35,     53,    53 }, // 2760: b46P60; gps060

    // Amplitude begins at 3406.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 867,868, 35,     46,    46 }, // 2761: b46P61; gps061

    // Amplitude begins at  973.4, peaks 1293.0 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 874,875, 35,     20,    20 }, // 2762: b46P65; gps065

    // Amplitude begins at 1132.5,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 149,876, 35,     53,    53 }, // 2763: b46P66; gps066

    // Amplitude begins at 1744.3,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 877,878, 35,    113,   113 }, // 2764: b46P67; gps067

    // Amplitude begins at   20.5, peaks 1418.4 at 0.0s,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 885,886, 35,    440,   440 }, // 2765: b46P71; gps071

    // Amplitude begins at  991.7,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 863,864, 35,     20,    20 }, // 2766: b46P58; gps058

    // Amplitude begins at 3977.2,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 872,873, 35,     40,    40 }, // 2767: b46P64; gps064

    // Amplitude begins at 1373.8,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 879,880, 35,    140,   140 }, // 2768: b46P68; gps068

    // Amplitude begins at  283.5, peaks  427.8 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 883,884, 35,     20,    20 }, // 2769: b46P70; gps070

    // Amplitude begins at    2.8, peaks 3133.5 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 897,898, 35,    220,   220 }, // 2770: b46P79; gps079

    // Amplitude begins at 1884.6,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 899,900, 35,    226,   226 }, // 2771: b46P80; gps080

    // Amplitude begins at 2100.8,
    // fades to 20% at 1.3s, keyoff fades to 20% in 1.3s.
    { 901,902, 35,   1293,  1293 }, // 2772: b46P81; gps081

    // Amplitude begins at  985.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 889,890, 35,     20,    20 }, // 2773: b46P73; gps073

    // Amplitude begins at  985.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 889,891, 35,     20,    20 }, // 2774: b46P74; gps074

    // Amplitude begins at    0.0, peaks 2433.6 at 0.1s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 895,896, 35,    246,   246 }, // 2775: b46P78; gps078

    // Amplitude begins at  787.9,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 838,839, 35,    166,   166 }, // 2776: b46P39; gps039

    // Amplitude begins at 1145.3, peaks 1216.9 at 0.0s,
    // fades to 20% at 3.6s, keyoff fades to 20% in 0.0s.
    { 847,848, 35,   3580,     0 }, // 2777: b46P49; b47P57; b47P59; gpo057; gpo059; gps049

    // Amplitude begins at 1153.9, peaks 1196.1 at 0.0s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 851,852, 35,    593,   593 }, // 2778: b46P52; gps052

    // Amplitude begins at 1144.5, peaks 1152.8 at 0.0s,
    // fades to 20% at 40.0s, keyoff fades to 20% in 0.0s.
    { 861,862, 35,  40000,     0 }, // 2779: b46P57; gps057

    // Amplitude begins at 1379.7, peaks 1879.3 at 0.1s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 1919,1920, 35,    626,   626 }, // 2780: b47P41; b47P42; b47P43; b47P44; b47P45; b47P46; b47P47; b47P48; b47P49; b47P50; b47P51; b47P52; b47P53; gpo041; gpo042; gpo043; gpo044; gpo045; gpo046; gpo047; gpo048; gpo049; gpo050; gpo051; gpo052; gpo053

    // Amplitude begins at    0.0, peaks  495.5 at 2.9s,
    // fades to 20% at 4.4s, keyoff fades to 20% in 4.4s.
    { 2882,824, 35,   4373,  4373 }, // 2781: b47P88; gpo088

    // Amplitude begins at 2318.9, peaks 2569.4 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1914,1915, 35,     53,    53 }, // 2782: b47P35; b47P36; gpo035; gpo036

    // Amplitude begins at 1012.9, peaks 1537.2 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1918,837, 35,     73,    73 }, // 2783: b47P38; b47P40; gpo038; gpo040

    // Amplitude begins at 2625.1,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 892,892, 35,     60,    60 }, // 2784: b47P39; b47P75; b47P76; b47P77; b47P85; gpo039; gpo075; gpo076; gpo077; gpo085

    // Amplitude begins at 2948.6,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1916,1917, 35,     80,    80 }, // 2785: b47P37; gpo037

    // Amplitude begins at 1289.3, peaks 2764.3 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 855,1921, 35,    126,   126 }, // 2786: b47P54; gpo054

    // Amplitude begins at 1802.6, peaks 1903.8 at 0.0s,
    // fades to 20% at 3.4s, keyoff fades to 20% in 0.0s.
    { 1922,1923, 35,   3373,    33 }, // 2787: b47P55; gpo055

    // Amplitude begins at  695.7,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 863,863, 35,     20,    20 }, // 2788: b47P58; gpo058

    // Amplitude begins at 1237.1,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1924,1925, 35,     20,    20 }, // 2789: b47P60; gpo060

    // Amplitude begins at 2396.8, peaks 2624.4 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1926,1927, 35,     66,    66 }, // 2790: b47P61; gpo061

    // Amplitude begins at 2445.0,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1928,1928, 35,     20,    20 }, // 2791: b47P62; b47P86; gpo062; gpo086

    // Amplitude begins at 2674.1,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1929,1929, 35,     33,    33 }, // 2792: b47P63; b47P87; gpo063; gpo087

    // Amplitude begins at 2002.2,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 911,911, 35,     40,    40 }, // 2793: b47P64; gpo064

    // Amplitude begins at  770.6, peaks  821.6 at 0.0s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 1930,1930, 35,    173,   173 }, // 2794: b47P65; gpo065

    // Amplitude begins at  851.7,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1931,1931, 35,      6,     6 }, // 2795: b47P66; gpo066

    // Amplitude begins at  774.6,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1932,1932, 35,     53,    53 }, // 2796: b47P67; gpo067

    // Amplitude begins at 1412.2,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 1933,1933, 35,     53,    53 }, // 2797: b47P68; gpo068

    // Amplitude begins at    0.8, peaks  371.6 at 0.0s,
    // fades to 20% at 0.1s, keyoff fades to 20% in 0.1s.
    { 881,881, 35,    106,   106 }, // 2798: b47P69; b47P82; gpo069; gpo082

    // Amplitude begins at  149.2, peaks  322.9 at 0.0s,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1934,1934, 35,     20,    20 }, // 2799: b47P70; gpo070

    // Amplitude begins at   13.7, peaks  367.3 at 0.0s,
    // fades to 20% at 0.4s, keyoff fades to 20% in 0.4s.
    { 1935,1935, 35,    393,   393 }, // 2800: b47P71; gpo071

    // Amplitude begins at   14.9, peaks  365.3 at 0.0s,
    // fades to 20% at 0.8s, keyoff fades to 20% in 0.8s.
    { 1936,1936, 35,    806,   806 }, // 2801: b47P72; gpo072

    // Amplitude begins at  661.3,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1937,890, 35,     20,    20 }, // 2802: b47P73; gpo073

    // Amplitude begins at  661.3,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 1937,891, 35,     20,    20 }, // 2803: b47P74; gpo074

    // Amplitude begins at    0.0, peaks 2433.6 at 0.1s,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 1938,1939, 35,    246,   246 }, // 2804: b47P78; gpo078

    // Amplitude begins at    0.7, peaks 2077.7 at 0.1s,
    // fades to 20% at 0.3s, keyoff fades to 20% in 0.3s.
    { 1940,1941, 35,    326,   326 }, // 2805: b47P79; gpo079

    // Amplitude begins at 3596.2,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 1942,1943, 35,    153,   153 }, // 2806: b47P80; gpo080

    // Amplitude begins at 3631.3, peaks 3791.8 at 0.0s,
    // fades to 20% at 0.6s, keyoff fades to 20% in 0.6s.
    { 1944,1945, 35,    553,   553 }, // 2807: b47P81; gpo081

    // Amplitude begins at 2324.3, peaks 2420.5 at 0.0s,
    // fades to 20% at 1.1s, keyoff fades to 20% in 1.1s.
    { 2883,2884, 35,   1073,  1073 }, // 2808: b47P83; gpo083

    // Amplitude begins at 1367.7,
    // fades to 20% at 0.2s, keyoff fades to 20% in 0.2s.
    { 2885,2886, 35,    233,   233 }, // 2809: b47P84; gpo084

    // Amplitude begins at 1146.3,
    // fades to 20% at 0.0s, keyoff fades to 20% in 0.0s.
    { 859,859, 35,     26,    26 }, // 2810: b47P56; gpo056

};
const char* const banknames[48] =
{
    "AIL (Star Control 3, Albion, Empire 2, Sensible Soccer, Settlers 2, many others)",
    "HMI (Descent, Asterix)",
    "HMI (Descent:: Int)",
    "HMI (Descent:: Ham)",
    "HMI (Descent:: Rick)",
    "DMX (Doom)",
    "DMX (Hexen, Heretic)",
    "AIL (Warcraft 2)",
    "AIL (SimFarm, SimHealth :: Quad-op)",
    "AIL (SimFarm, Settlers, Serf City)",
    "AIL (Air Bucks, Blue And The Gray, America Invades, Terminator 2029)",
    "AIL (Ultima Underworld 2)",
    "AIL (Caesar 2)",
    "AIL (Death Gate)",
    "AIL (Kasparov's Gambit)",
    "AIL (High Seas Trader)",
    "AIL (Discworld, Grandest Fleet, Pocahontas, Slob Zone 3d, Ultima 4)",
    "AIL (Syndicate)",
    "AIL (Guilty, Orion Conspiracy, Terra Nova Strike Force Centauri)",
    "AIL (Magic Carpet 2)",
    "AIL (Jagged Alliance)",
    "AIL (When Two Worlds War)",
    "AIL (Bards Tale Construction)",
    "AIL (Return to Zork)",
    "AIL (Theme Hospital)",
    "AIL (Inherit The Earth)",
    "AIL (Inherit The Earth, file two)",
    "AIL (Little Big Adventure)",
    "AIL (Wreckin Crew)",
    "AIL (FIFA International Soccer)",
    "AIL (Starship Invasion)",
    "AIL (Super Street Fighter 2)",
    "AIL (Lords of the Realm)",
    "AIL (Syndicate Wars)",
    "AIL (Bubble Bobble Feat. Rainbow Islands, Z)",
    "AIL (Warcraft)",
    "AIL (Terra Nova Strike Force Centuri)",
    "AIL (System Shock)",
    "AIL (Advanced Civilization)",
    "AIL (Battle Chess 4000)",
    "AIL (Ultimate Soccer Manager)",
    "HMI (Theme Park)",
    "HMI (3d Table Sports, Battle Arena Toshinden)",
    "HMI (Aces of the Deep)",
    "HMI (Earthsiege)",
    "HMI (Anvil of Dawn)",
    "AIL (Master of Magic, Master of Orion 2 :: std perccussion)",
    "AIL (Master of Magic, Master of Orion 2 :: orchestral percussion)",
};
const unsigned short banks[48][256] =
{
    { // bank 0, AIL (Star Control 3, Albion, Empire 2, Sensible Soccer, Settlers 2, many others)
  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15,
 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
 32, 33, 34, 35, 36, 37, 38, 33, 39, 40, 41, 42, 43, 44, 45, 46,
 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62,
 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78,
 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94,
 95, 96, 97, 98, 99,100,101,102,103,104,105,106,107,108,109,110,
111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,127,127,128,129,130,129,131,132,131,133,131,134,131,
131,135,131,136,137,138,139,140,141,135,142,136,143,144,145,146,
147,148,149,150,151,152,153,154,155,156,157,158,159,160,161,162,
163,164,165,166,167,168,169,131,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
    },
    { // bank 1, HMI (Descent, Asterix)
170,171,172,173,174,175,176,177,178,179,180,181,182,183,184,185,
 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26,186, 28,170, 30, 31,
 32, 33, 34, 35, 36,187, 38, 33, 39, 40, 41, 42, 43, 44, 45, 46,
 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60,188, 62,
 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78,
 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94,
 95, 96, 97, 98, 99,100,101,102,103,104,105,106,107,108,109,110,
111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,
189,189,189,189,189,189,189,189,189,189,189,189,189,189,189,189,
189,189,189,189,189,189,189,189,189,189,189,190,191,192,193,194,
195,196,197,198,199,200,201,202,203,204,205,206,205,207,208,209,
210,211,212,213,211,213,214,211,215,211,216,217,218,219,220,221,
222,223,224,225,226,227,228,229,230,231,232,233,234,235,236,237,
238,239,227,240,241,242,200,243,189,189,189,189,189,189,189,189,
189,189,189,189,189,189,189,189,189,189,189,189,189,189,189,189,
189,189,189,189,189,189,189,189,189,189,189,189,189,189,189,189,
    },
    { // bank 2, HMI (Descent:: Int)
244,244,244,244,244,244,244,244,244,244,244,244,244,244,244,244,
244,244,244,244,244,244,244,244,244,244,244,259,260,261,262,263,
264,265,266,267,119,268,269,270,271,272,273,274,275,276,277,278,
279,280,244,244,244,244,244,244,244,244,244,244,244,244,244,244,
244,244,244,244,244,244,244,244,244,244,244,244,244,244,244,244,
244,244,244,244,244,244,244,244,244,244,244,244,244,244,244,244,
244,244,244,244,244,244,244,244,244,244,244,244,244,244,244,244,
244,244,244,244,244,244,244,244,244,244,244,244,244,244,244,244,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,281,282,283,284,285,
286,287,288,289,290,291,292,293,294,295,296,297,298,299,300,301,
302,303,304,305,306,307,308,309,310,311,312,313,314,315,316,317,
318,319,320,321,322,323,324,325,326,327,328,329,330,331,332,333,
334,335,336,337,338,339,340,341,342,343,344,345,346,347,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
    },
    { // bank 3, HMI (Descent:: Ham)
244,245,246, 28,247, 31, 30,248,249,250,251,252,253, 38, 46,254,
 79, 84,255,256,244,244,244,244,244,244,244,259,260,261,262,263,
264,265,266,267,119,268,269,270,271,272,273,274,275,276,277,278,
279,244,112, 99,348,349, 93,350,351,107,116,352, 28, 78,353,354,
355, 79, 94, 38, 33,115,356,357,358,359,244,244,244,244,244,244,
244,244,244,244,244,244,244,244,244,244,244,244,244,244,244,244,
244,244,244,244,244,244,244,244,244,244,244,244,244,244,244,244,
244,244,244,244,244,244,244,244,244,244,244,244,244,244,244,198,
366,367,368,369,370,371,372,373,374,375,376,377,378,379,380,381,
382,383,384,385,198,198,198,198,198,198,198,281,282,283,284,285,
286,287,288,289,290,291,292,293,294,386,387,388,389,299,300,301,
302,303,304,305,390,307,308,309,310,311,312,313,391,315,316,317,
318,319,320,392,393,323,324,325,326,394,395,329,330,331,332,333,
334,335,336,337,338,339,340,341,396,343,397,345,346,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
    },
    { // bank 4, HMI (Descent:: Rick)
244,244,244,244,244,244,244,244,244,244,244,244,244, 38, 46,254,
 79, 84,255,256, 49, 89, 92, 93,105,257,258,259,260,261,262,263,
264,265,266,267,119,268,269,270,271,272,273,274,275,276,277,278,
279,244,244,244,244,244,244,244,244,244,244,244,244,244,244,244,
244,244,244,244,244,244,244,244,244,244,244,244,398,399,400,401,
402, 34,403,404,248, 51, 52, 84,405,406,407,408,409, 85,348, 90,
 93, 94,101,410,114,119,244,244,244,244,244,244,244,244,244,244,
244,244,244,244,244,244,244,244,244,244,244,244,244,244,244,244,
198,198,198,198,198,198,198,198,198,198,198,198,198,411,377,378,
380,412,413,412,413,414,415,416,417,418,419,281,282,283,284,285,
286,287,288,289,290,291,292,293,294,295,296,297,298,299,300,301,
302,303,304,305,306,307,308,309,310,311,312,313,314,315,316,317,
318,319,320,321,322,323,324,325,326,327,328,329,330,331,332,333,
334,335,336,337,338,339,340,341,342,343,344,345,346,420,421,383,
422,423,374,424,376,425,426,427,428,429,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
    },
    { // bank 5, DMX (Doom)
430,431,432,433,434,435,436,437,438,439,440,441,442,443,444,445,
446,447,448,449,450,451,452,453,454,455,456,457,458,459,460,461,
462,463,464,465,466,467,468,469,470,471,472,473,474,475,476,477,
478,479,480,481,482,483,484,485,486,487,488,489,490,491,492,493,
494,495,496,497,498,499,500,501,502,503,504,505,506,507,508,509,
510,511,512,513,514,515,516,517,517,518,519,520,521,522,523,524,
525,526,527,528,529,530,531,532,533,534,535,536,537,538,539,540,
541,542,543,544,545,546,547,548,549,550,551,552,553,554,555,556,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,557,558,559,560,561,562,563,564,565,566,567,568,569,
570,571,572,573,574,575,576,577,578,571,579,573,580,581,582,583,
584,585,586,587,588,566,589,590,590,590,590,591,592,593,594,590,
595,596,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
    },
    { // bank 6, DMX (Hexen, Heretic)
430,431,432,433,434,435,436,437,438,439,440,441,442,443,444,445,
446,447,448,449,450,451,452,453,454,455,456,457,458,597,598,461,
599,463,464,600,601,467,468,469,470,471,472,473,474,475,476,477,
478,479,480,481,482,483,484,485,486,487,602,489,490,491,492,493,
494,495,496,497,498,499,500,501,502,503,504,505,506,507,508,509,
510,511,512,513,514,515,516,517,517,518,519,520,521,522,523,524,
525,526,527,528,529,530,531,532,533,534,535,536,537,538,539,540,
541,542,543,544,545,546,547,548,549,550,551,552,553,554,555,556,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,557,558,559,560,561,603,563,564,565,566,567,568,569,
570,571,572,573,574,575,576,604,578,571,579,573,580,581,582,583,
584,585,586,587,588,566,589,590,590,590,590,591,592,593,594,590,
595,596,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
    },
    { // bank 7, AIL (Warcraft 2)
  0,  1,  2,  3,  4,  5,605,  7,  8,606, 10, 11, 12, 13,607, 15,
 16, 17, 18,608, 20, 21, 22, 23,609, 25, 26, 27, 28, 29, 30, 31,
 32, 33, 34, 35, 36, 37, 38, 33, 39, 40, 41, 42,610,611,612,613,
614,615,616, 50,617, 52, 53,618,619,620,621, 58,622,623, 61, 62,
 63, 64, 65, 66,624, 68,625,626,627,628, 73, 74, 75, 76, 77, 78,
 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93,629,
 95, 96, 97, 98, 99,100,101,102,103,104,105,106,107,108,109,110,
111,112,113,114,630,116,631,632,119,120,121,122,123,124,125,126,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,633,633,634,635,636,635,637,638,637,639,637,640,637,
637,641,637,136,642,138,643,644,141,645,646,136,143,144,145,146,
147,148,149,150,151,647,153,154,155,156,157,158,648,160,161,162,
163,164,165,166,167,168,169,131,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
    },
    { // bank 8, AIL (SimFarm, SimHealth :: Quad-op)
649,650,651,652,653,654,655,656,657,658,659,660,661,662,663,664,
665,666,667,668,669,670,671,672,673,674,675,676,677,678,679,680,
681,682,683,684,685,686,687,688,689,690,691,692,693,694,695,696,
697,698,699,700,701,702,703,704,705,706,707,708,709,710,711,712,
713,714,715,716,717,718,719,720,721,722,723,724,725,726,727,728,
729,730,731,732,733,734,735,736,737,738,739,740,741,742,743,744,
745,746,747,748,749,750,751,752,753,754,755,756,757,758,759,760,
761,762,763,764,765,766,767,768,769,770,771,772,773,774,775,776,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,127,127,777,778,779,780,781,782,781,783,781,784,781,
781,785,781,786,787,788,789,790,791,792,793,786,794,795,796,797,
798,799,800,801,802,803,804,805,806,807,808,809,810,810,811,812,
813,814,815,816,817,818,819,820,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
    },
    { // bank 9, AIL (SimFarm, Settlers, Serf City)
821,171,172,822,174,175,176,177,823,179,180,824,182,183,825,826,
827,828,829,830,831,832,833,834,835, 25, 26,186, 28,170, 30,836,
837, 33, 34, 35, 36,187, 38, 33, 39, 40,838, 42, 43, 44, 45, 46,
839,840,841,842,843,844,845,846, 55, 56,847, 58, 59,848,849, 62,
850,851,852,853, 67, 68, 69, 70, 71, 72,854, 74,855,856,857,858,
 79, 80, 81,859, 83,860,861, 86,862,863,864,865,866,867,868,869,
870, 96, 97,871,872,873,874,875,103,104,105,876,107,108,109,110,
111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,127,127,128,129,130,129,131,132,131,133,131,134,131,
131,135,131,136,137,138,139,877,141,135,878,136,143,144,145,146,
147,148,149,150,151,152,153,154,155,879,880,158,159,160,161,881,
163,164,165,166,167,168,169,131,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
    },
    { // bank 10, AIL (Air Bucks, Blue And The Gray, America Invades, Terminator 2029)
882,883,884,883,885,883,886,882,887,888,888,889,890,890,890,891,
892,892,892,893,893,893,894,894,895,895,895,895,896,896,896,896,
897,898,899,900,898,352,901,902,903,903,904,905,896,896,906,907,
908,909,908,910,911,912,911,913,914,915,916,917,917,918,919,920,
921,896,896,896,921,921,905,905,922,922,922,922,923,923,924,924,
924,925,926,927,928,928,929,930,931,931,932,933,934,935,936,937,
938,939,940,941,942,942,942,943,941,910,944,945,945,945,923,923,
946,947,948,949,949,948,948,950,951,952,953,954,955,898,903,956,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,957,957,958,959,959,959,958,960,958,961,958,961,958,
958,962,958,961,198,198,961,198,958,198,198,198,958,958,958,958,
958,958,958,959,959,961,961,961,961,961,198,961,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
    },
    { // bank 11, AIL (Ultima Underworld 2)
963,963,963,964,965,966,967,968,969,970,971,889,972,973,974,975,
976,976,976,977,977,977,978,979,980,981,895,982,896,896,896,896,
983,984,985,986,987,352,988,902,989,990,904,991,896,896,906,992,
993,994,995,996,911,997,911,998,999, 45, 45,1000,1001,1002,1003,1004,
1005,896,896,896,921,921,1006,1006,1007,1008,1008,1009,1010,923,1011,1012,
1013,1014,1015,1016,1017,1018,1019,930,1020,1020,1021,1021,1022,1022,1023,937,
938,1024,1024,941,942,942,942,943,941,910,944,1025,945,945,923,923,
1026,1027,1028,1029,1030,1031,948,1030,1031,1032,1033,954,1034,1035,1036,956,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,1037,1037,1031,1038,1031,1038,1031,1039,1031,1040,1031,1041,1031,
1031,135,1031,1042,198,198,1043,198,198,198,198,198,1044,144,1045,1045,
1045,198,198,198,198,1046,1046,198,198,198,198,158,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
    },
    { // bank 12, AIL (Caesar 2)
1047,1048,1049,1050,1051,1052,1053,893,1054,1055,1056,1057,1058,1059,1060,1061,
1062,1063,1064,1065,1066,1067,1068,1069,1070,1071,1072,918,1073,1074,1074,1074,
1075,1076,1077,1078,1079,1079,1080,1081,1082,1083,1084,198,1074,1085,1086,1087,
1088,1089,1074,1074,1074,1074,1074,1074,1090,1091,1092,1093,1094,408,407,937,
1095,1074,1074,1074,1096,1097,1098,1099,1100,1101,1102,1074,198,1103,198,198,
198,198,198,198,1104,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,1105,1106,198,198,198,1107,
198,198,198,1108,1087,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,1109,
198,1110,198,1111,1109,1112,1113,1114,1115,1116,1117,1116,1118,1116,1118,1116,
1116,1119,1116,1120,198,198,1118,198,1121,198,198,198,1124,1124,198,1125,
1126,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
    },
    { // bank 13, AIL (Death Gate)
1127,1128,1129,1130,1131,1132,1133,1134,1135,1136,1137,1138,1139,1140,1141,1142,
1143,1144,1145,1146,1147,1148,1149,1150,1151,1152,1153,1154,1155,1156,1157,1158,
1159,1160,1161,1162,1163,1164,1165,1166,1167,1168,1169,1170,1171,1172,1173,1174,
1175,1176,1177,1178,1179,1180,1181,1182,1183,1184,1185,1186,1187,1188,1189,1190,
1191,1192,1193,1194,1195,1196,1197,1198,1199,1200,1201,1202,1203,1204,1205,1206,
1207,1208,1209,1210,1211,1212,1213,1214,1215,1216,1217,1218,1219,1220,1221,1222,
1223,1224,1225,1226,1227,1228,1229,1230,1231,1232,1233,1234,1235,1236,1237,1238,
1239,1240,1241,1242,1243,1244,1245,1246,1247,1248,1249,1250,1251,1252,1253,1254,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,1255,1255,1256,1257,1258,1259,1260,1261,1260,1262,1260,1263,1260,
1260,1264,1260,1265,1266,1265,1267,1268,1269,1264,1270,1265,1271,1272,1273,1274,
1274,1275,1275,1276,1276,1277,1278,1279,1280,1281,1282,1283,1284,1284,1285,1286,
1287,1288,1289,1290,1290,1291,1292,1293,1294,1295,1296,1297,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
    },
    { // bank 14, AIL (Kasparov's Gambit)
1298,1299,1300,1301,1302,1303,1304,1305,1306,1307,1308,1309,1310,1311,1312,832,
976,976,976,977,977,977,978,979,1313,1314,1315,1316, 34,1317, 33,1318,
1319,984,985,1320,987,1321,988,1322,989,1323,1324,991,1325,1326,1327,992,
1328,994,1329,996,1330,1331,1332,1333,1334, 45, 45,1335,1336,1337,1338,1339,
1005,896,1340,1341,1342,1343,1006,1006,1007,1008,1008,1009,1010,1344,1345,1346,
1347,1348,1015,1016,1017,1018,1019,1349,1350,1351,1352,1353,1354,1355,1356,1357,
1358,1024,1359,1360,1361,1362,1363,1364,1365,1366,944,1025,1367,1368,1369,1370,
1371,1372,1373,1374,1375,1376,1377,1378,1379,1380,1381,1382,122,1383,1384,1385,
1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,
1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,
1386,1386,1386,1387,1387,1388,1389,1390,1391,1392,1393,1392,1040,1392,1041,1392,
1392,135,1392,167,1386,1386,1043,1386,1394,1386,1386,1386,1044,144,1395,169,
169,1396,1397,1398,1399,152,153,154,155,1400,1386,158,1386,1386,1386,1386,
1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,
1386,1386,1386,1386,198,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,
1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,
    },
    { // bank 15, AIL (High Seas Trader)
1047,1048,1049,1050,1051,1052,1053,893,1054,1055,1056,1057,1058,1059,1060,1061,
1062,1063,1064,1065,1066,1067,1068,1069,1070,1071,1072,918,1073,1074,1074,1074,
1075,1076,1077,1078,1079,1079,1080,1081,1082,1401,1084,198,1074,1085,1402,1087,
1403,1089,1074,1074,1074,1074,1074,1074,1404,1091,1405,1093,1406,408,407,937,
1095,1074,1074,1074,1096,1097,1098,1099,1100,1101,1102,1074,198,1103,198,198,
198,198,198,198,1104,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,1105,1106,198,198,198,1107,
198,198,198,1108,1087,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,1109,
198,1110,198,1407,1109,1112,1113,1114,1115,1116,1118,1116,1118,1116,1118,1116,
1116,1119,1116,1120,198,198,1118,198,1121,198,198,198,1124,1124,198,1125,
1125,198,198,198,198,198,1408,198,198,1409,1410,198,198,198,198,198,
198,198,198,198,198,198,198,1411,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
    },
    { // bank 16, AIL (Discworld, Grandest Fleet, Pocahontas, Slob Zone 3d, Ultima 4)
821,171,172,822,174,175,176,177,823,179,180,824,182,183,825,826,
827,828,829,830,831,832,833,834,835, 25, 26,186, 28,170, 30,836,
837, 33, 34, 35, 36,187, 38, 33, 39, 40,838, 42, 43, 44, 45, 46,
839,840,841,842,843,844,845,846, 55, 56,847, 58, 59,848,849, 62,
850,851,852,853, 67, 68, 69, 70, 71, 72,854, 74,855,856,857,858,
 79, 80, 81,859, 83,860,861, 86,862,863,864,865,866,867,868,869,
870, 96, 97,871,872,873,874,875,103,104,105,876,107,108,109,110,
111,112,113,114,115,116,117,1412,119,120,121,122,123,124,125,126,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,127,127,128,129,130,129,131,132,131,133,131,134,131,
131,135,131,136,137,138,139,877,141,135,878,136,143,144,145,146,
147,148,149,150,151,152,153,154,155,879,880,158,159,160,161,881,
163,164,165,166,167,168,169,131,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
    },
    { // bank 17, AIL (Syndicate)
882,883,1413,1414,1415,883,886,882,1416,888,1417,1418,1419,1420,1414,1421,
892,892,892,1422,893,1423,894,894,1424,1425,1426,895,896,896,896,896,
1427,1428,899,900,1428,352,1429,902,903,1430,904,1431,896,896,906,907,
1432,1433,1432,1434,911,912,911,913,914,915,916,917,917,918,919,891,
921,896,896,896,921,921,905,1435,1436,1436,1436,1436,1437,1438,924,924,
924,1439,1440,927,928,928,929,1441,931,931,932,933,934,1442,1433,937,
1443,1444,940,941,1445,942,942,1446,941,1434,944,1442,1442,1442,923,923,
946,1447,948,1448,949,948,948,950,951,952,953,954,1449,1450,1451,1452,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,1453,1453,958,1454,959,958,958,1455,958,961,958,961,961,
1456,1457,958,198,1456,1456,198,198,958,198,198,198,1454,958,958,1457,
958,958,958,1457,1457,961,961,961,961,961,198,961,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
    },
    { // bank 18, AIL (Guilty, Orion Conspiracy, Terra Nova Strike Force Centauri)
649,650,651,652,653,654,655,656,657,658,659,660,661,662,663,664,
665,666,667,668,669,670,671,672,673,674,675,676,677,678,679,680,
681,682,683,684,685,686,687,688,689,690,691,692,693,694,695,696,
697,698,699,700,701,702,703,704,705,706,707,708,709,710,711,712,
713,714,715,716,717,718,719,720,721,722,723,724,725,726,727,728,
729,730,731,732,733,734,735,736,737,738,739,740,741,742,743,744,
745,746,747,748,749,750,751,752,753,754,755,756,757,758,759,760,
761,762,763,764,765,766,767,768,769,770,771,772,773,774,775,776,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,127,127,777,778,779,780,781,782,781,783,781,784,781,
781,785,781,786,787,788,789,790,791,792,793,1458,794,795,796,797,
798,799,800,801,802,803,804,805,806,807,808,809,810,810,811,812,
813,814,815,816,817,818,819,820,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
    },
    { // bank 19, AIL (Magic Carpet 2)
882,883,1413,1459,1460,1461,886,882,1416,1462,1463,1464,1419,1465,1414,1421,
1466,1467,892,1422,893,1468,1469,1470,1424,1471,1426,1472,1473,1474,896,896,
1427,1475,1476,1477,1478,1479,1480,1481,1482,1430,1483,1484,1485,1486,1487,1488,
1489,1490,1491,1492,1493,1494,1469,1495,914,915,916,917,917,1496,1497,1498,
1499,1500,1501,1502,1503,1504,1505,1506,1436,1507,1508,1436,1509,1510,1511,1512,
1513,1514,1515,1516,1517,1517,1518,1514,1496,1519,1519,1519,1519,1519,1520,1521,
1522,1444,1523,1524,1525,1526,1526,1527,1524,1434,1528,1529,1529,1529,1530,1530,
946,1447,1531,1532,949,948,948,1533,1534,1535,1536,954,1537,1450,1451,1452,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,1538,1539,1540,1541,1541,1541,958,1542,958,1543,958,1544,1543,
1456,1457,958,198,1456,1456,198,198,958,198,198,198,1454,958,958,1545,
958,958,958,1457,1457,1543,1539,1543,1546,1546,198,1546,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
    },
    { // bank 20, AIL (Jagged Alliance)
1547,198,884,883,885,883,886,882,887,888,888,889,890,890,890,891,
892,892,892,893,893,893,894,894,1548,1549,1550,1550,896,896,1551,896,
897,898,899,900,898,352,901,902,903,903,904,905,896,896,906,907,
1552,909,1553,1554,911,912,911,913,914,915,916,917,917,918,919,920,
1549,1555,1556,198,1550,198,905,1557,1558,198,198,198,1559,1560,198,924,
1561,1562,198,198,198,198,1563,930,1564,931,932,1565,1566,1566,1567,1568,
938,1569,1570,1571,1572,198,942,1573,1570,1568,198,1574,198,198,198,1574,
1575,198,198,198,1576,1577,198,1578,198,198,1556,198,955,1547,903,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,1579,1580,1581,1582,1581,958,1583,958,961,958,961,958,
958,1584,958,1585,198,1581,128,1581,198,198,198,198,958,958,958,958,
958,958,958,1581,132,961,132,961,961,961,198,961,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
    },
    { // bank 21, AIL (When Two Worlds War)
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
1586,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,1587,198,198,198,198,198,198,198,198,1588,198,198,198,198,198,
198,198,198,198,198,198,198,198,1589,198,198,1590,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
    },
    { // bank 22, AIL (Bards Tale Construction)
267,1591,198,198,198,198,198,198,198,198,198,198,1592,1592,1592,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,1593,1594,198,198,198,1595,198,198,198,198,198,198,198,198,198,
1596,1597,198,198,198,198,198,198,198,915,1598,1599,1599,198,198,198,
198,198,198,198,198,198,1600,1600,1601,1602,1601,198,198,198,198,198,
198,198,1603,1603,198,198,198,198,198,1604,933,198,934,198,198,198,
198,198,198,198,198,198,1605,1595,1606,1607,1608,1609,198,198,1610,1611,
270,1612,1613,198,198,1614,198,198,1615,198,198,1616,1617,1618,1619,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,1620,1620,1620,198,198,198,198,198,198,198,
198,1621,198,198,198,198,1622,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
    },
    { // bank 23, AIL (Return to Zork)
1298,1299,1300,1301,1302,1303,1304,1305,1306,1307,1308,1309,1310,1311,1312,832,
976,976,976,977,977,977,978,979,1623,1314,1315,1316, 34,1317, 33,1318,
1319,984,985,1320,987,1321,988,1322,989,1323,1324,991,1325,1326,1327,992,
1328,994,1329,996,1330,1331,1332,1333,1334, 45, 45,1335,1336,1337,1338,1339,
1005,896,1340,1341,1342,1343,1006,1006,1007,1008,1008,1009,1010,1344,1345,1346,
1347,1348,1015,1016,1017,1018,1019,1349,1020,1351,1352,1353,1022,1022,1356,1357,
1358,1024,1359,1360,1361,1362,1363,1364,1365,1366,944,1025,1367,1368,1369,1370,
1371,1372,1373,1374,1375,1376,1377,1378,1379,1380,1381,1382,122,1383,1384,1385,
1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,
1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,
1386,1386,1386,1387,1387,1388,1389,1390,1391,1392,1393,1392,1040,1392,1041,1392,
1392,135,1392,167,1386,1386,1043,1386,1394,1386,1386,1386,1044,144,1395,169,
169,1396,1397,1398,1399,152,153,154,155,1400,1386,158,1386,1386,1386,1386,
1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,
1386,1386,1386,1386,198,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,
1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,1386,
    },
    { // bank 24, AIL (Theme Hospital)
1624,1624,1624,1624,1624,1624,1467,1625,1626,1627,1628,1629,1630,1631,1632,1633,
1634,1634,1634,1634,1635,1636,1637,1636,1638,1639,1638,1638,1638,1640,1497,1641,
1642,1642,1642,1642,1642,1642,1642,1642,1643,1643,1643,1642,1643,1644,1645,1646,
1643,1643,1647,1647,1653,1648,1648,1649,1650,1651,1652,1650,1654,1650,1650,1650,
1650,1650,1650,1650,1470,1654,1655,1656,1657,1657,1657,1658,1657,1657,1659,1659,
1660,1439,1661,1661,1624,1648,1662,1663,1664,1665,1666,1667,1668,1669,1670,1662,
1670,1671,1632,1672,1673,1674,1675,1676,1677,1678,1679,1680,1681,1682,1683,1683,
1626,1684,1685,1686,1687,1687,1687,1688,1689,1661,1690,1691,1692,1693,1694,1451,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,1538,1538,1538,1538,1538,
1457,1457,1457,1538,1453,1457,1454,1454,1454,1546,1695,1546,1696,1546,1696,1546,
1546,1697,1546,1542,1697,1542,1542,1543,1698,1697,1699,1542,1546,1546,1546,1546,
1546,1546,1546,1457,1457,1700,1700,1543,1546,1546,1546,1457,1701,1701,1701,1701,
1545,1545,1700,1545,1545,1457,1546,1546,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
    },
    { // bank 25, AIL (Inherit The Earth)
  0,  1,  2,198,198,198,  5,1702,198,198,829,198,830,830,1703,832,
198,198,198,198,198,198,1704,1704,1705,1705, 62,1705, 33, 33, 38, 38,
862,1706,843,843, 96,871, 34,845,1707,870, 35,870, 62,198,827, 79,
841,842,1708, 44,198,198,838,838, 42, 45, 45,1709, 25,1710,198,103,
198,837,198, 34, 36, 37, 35, 35, 72, 72,198,198,1711, 74,198,198,
855,853, 70, 70, 67, 68, 69,833,198,198, 56,198, 59, 59,847,198,
198, 12,1707,198, 10,  9,1712, 13, 12,876,198,856,857,855,855,858,
1713,116,198,198,198,1714,115,1715,1716,1717,846,123,122,1715,1718,1715,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,127,127,128,129,130,129,131,132,131,133,131,134,131,
131,135,131,136,137,138,1719,877,141,135,878,136,143,144,145,146,
147,148,1720,150,151,152,153,154,155,879,880,158,159,160,161,881,
163,164,165,166,167,168,169,131,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
    },
    { // bank 26, AIL (Inherit The Earth, file two)
  0,  1,  2,198,198,198,  5,1702,198,198,829,198,830,830,830,832,
198,198,198,198,198,198,1704,1704,1705,1705, 62,1705, 33, 33, 38, 38,
862,843,843,843, 96,871, 34,845,1707,870, 35,870, 62,198,827, 79,
841,842,839, 44,198,198,838,838, 42, 45, 45,835, 25, 27,198,103,
198,837,198, 34, 36, 37, 35, 35, 72, 72,198,198,854, 74,198,198,
855,853, 70, 70, 67, 68, 69,833,198,198, 56,198, 59, 59,847,198,
198, 12,1707,198, 10,  9,1712, 13, 12,876,198,856,857,855,855,858,
 46,116,198,198,198,115,115,1715,1716,1717,846,123,122,1715,1718,1715,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,127,127,128,129,130,129,131,132,131,133,131,134,131,
131,135,131,136,137,138,139,877,141,135,878,136,143,144,145,146,
147,148,149,150,151,152,153,154,155,879,880,158,159,160,161,881,
163,164,165,166,167,168,169,131,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
    },
    // Bank 26 defines nothing new.
    { // bank 27, AIL (Little Big Adventure)
649,650,651,652,1721,654,655,656,1722,658,659,1723,661,662,1724,664,
665,666,667,668,669,670,671,672,673,674,675,676,677,678,679,680,
681,682,683,684,685,686,687,688,689,690,691,692,693,757,1725,1726,
1727,1727,1728,700,701,702,703,704,705,706,707,708,1729,1730,711,739,
713,714,715,716,1731,718,719,720,721,722,1732,724,725,726,727,728,
729,730,731,732,733,734,735,736,1733,738,739,740,741,742,743,744,
745,746,747,748,749,750,751,752,753,754,755,756,757,758,759,760,
761,762,763,764,765,766,767,768,769,770,771,772,773,774,775,776,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,127,
198,198,198,127,127,777,778,779,1734,781,782,781,783,781,784,781,
781,785,781,786,787,788,789,790,791,792,793,786,794,795,796,797,
798,799,800,801,802,803,804,805,806,807,808,809,810,810,811,812,
813,814,815,816,817,818,819,820,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
    },
    { // bank 28, AIL (Wreckin Crew)
882,883,884,883,885,883,886,882,887,1735,1735,1736,1737,1737,1737,891,
892,892,892,893,893,893,894,894,895,895,895,895,896,896,896,896,
1738,898,1739,1740,898,352,901,400,903,903,1741,905,896,896,1742,907,
1743,909,1743,910,1653,1082,1653,1495,914,915,916,917,917,918,919,920,
921,896,896,896,921,921,905,905,1101,1101,1101,1101,1744,1744,924,924,
924,1745,926,1516,1517,1517,1518,930,1746,1746,1747,1748,934,935,936,937,
1749,1750,1523,941,1526,1526,1526,943,941,910,1528,1751,1751,1751,1744,1744,
946,947,948,949,949,948,948,1533,1752,952,1536,954,955,898,903,1753,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,957,957,958,959,959,959,958,1754,958,1543,958,1543,958,
958,1755,958,1543,1543,198,1543,198,958,1755,198,198,958,958,958,958,
958,958,958,959,959,1543,1543,1543,1543,1543,198,1543,198,198,198,198,
1543,1543,1543,198,198,1543,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
    },
    { // bank 29, AIL (FIFA International Soccer)
  2,  2,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15,
 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
 32, 33, 34, 35, 36, 37, 38, 33, 39, 40, 41, 42, 43, 44, 45, 46,
 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62,
 63, 64, 65, 66, 67, 68, 69, 70, 71, 72,1756, 74, 75, 76, 77, 78,
 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94,
 95, 96, 97, 98, 99,100,101,102,103,104,105,106,107,108,109,110,
111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,
1757,132,1758,1759,1760,1760,1761,1762,1762,1763,1761,1764,1765,1766,1767,1766,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,127,127,128,129,130,129,131,132,131,133,131,134,131,
131,135,131,136,137,138,139,140,141,135,142,136,143,144,145,146,
147,148,149,150,151,152,153,154,155,156,157,158,159,160,161,162,
163,164,165,166,167,168,169,131,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
    },
    { // bank 30, AIL (Starship Invasion)
1768,1769,172,822,174,175,1770,1771,823,1772,180,1773,1774,183,825,1775,
889,887,1776,1777,831,832,930,834,349,1778, 26,1779, 28,170, 30,1780,
1781,1782,402,1783,399, 34, 38, 33,912,1784,1785, 42, 43,1786,1787,1788,
1789,1790,1791,1792,843,844,845,1793,931,933,1794, 58,1795,406,1796,1797,
850,851,852,853,1798, 68,929,927, 71,922,923, 74,855,1799,1800,858,
 79,356,351,859,352,860,861, 86,348,863,1801,865,866,867,868,869,
1802,1803, 97,1804,872,873,874,875,355,1805,1806,876,107,108,109,110,
903,112,113,114,115,116,948,1412,119,120,1807,955,1808,1809,125,1810,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,1811,
1811,1812,1812,127,1813,1814,1815,1816,1817,131,1818,131,133,131,1819,131,
131,135,131,136,137,138,1820,877,1821,1822,878,136,1823,1823,1824,1825,
1825,148,148,150,150,1826,153,154,155,879,1827,1828,159,160,161,881,
163,164,165,166,167,168,169,131,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
    },
    { // bank 31, AIL (Super Street Fighter 2)
649,650,651,652,653,654,655,656,657,658,659,660,661,662,663,664,
665,666,667,668,669,670,671,672,673,674,675,676,677,678,679,680,
681,682,683,684,685,686,687,688,689,690,691,692,693,694,695,696,
697,698,699,700,701,702,703,704,705,706,707,708,709,710,711,712,
713,714,715,716,717,718,719,720,721,722,723,724,725,726,727,728,
729,730,731,732,733,734,735,736,737,738,739,740,741,742,743,744,
745,746,747,748,749,750,751,752,753,754,755,756,757,758,759,760,
761,762,763,764,765,766,767,768,769,770,771,772,773,774,775,776,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,1829,1829,1830,1831,1832,1831,1833,1833,1833,1833,1833,1833,1833,
1833,1833,1833,1833,1833,1833,1834,1835,1836,785,1837,1833,1838,1839,1840,1841,
1842,1843,1844,1845,1846,1847,1848,1849,1850,1851,1852,1832,1832,1832,1853,1854,
1855,1856,1847,1857,817,1832,1840,1841,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
    },
    { // bank 32, AIL (Lords of the Realm)
882,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,1858,1858,1859,198,198,198,198,198,
198,198,198,198,198,198,198,198,1860,1401,198,198,198,198,1402,1087,
1861,1089,198,198,198,198,198,198,1862,1747,198,1093,1863,408,198,198,
198,198,198,198,1864,1865,1866,1516,1101,1101,1867,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,1109,
198,198,198,1868,1109,198,198,198,1115,1116,198,1116,198,1116,198,1116,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
    },
    { // bank 33, AIL (Syndicate Wars)
882,883,1413,1869,1870,1461,1871,882,1416,1872,1417,1873,1419,1465,1414,1421,
1466,1467,892,1422,893,1468,1469,1470,1424,1425,1426,1874,1473,896,896,896,
1427,1475,1739,1740,1475,352,1429,400,903,1430,1483,1484,896,896,1742,1875,
1491,1490,1491,1434,1653,1082,1653,1495,914,915,916,917,1876,1877,1497,891,
921,896,896,896,921,921,905,1435,1436,1436,1436,1436,1437,1510,1878,1878,
1878,1439,1440,1516,1517,1517,1518,1514,1496,1746,1747,1748,934,1529,1433,1521,
1879,1444,1880,1524,1525,1526,1526,1527,1524,1434,1528,1529,1529,1529,1530,1530,
946,1447,948,1881,949,948,948,1533,1534,1535,1536,954,1449,1450,1451,1452,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,1538,1453,1540,1454,959,958,958,1542,958,1543,958,1544,1543,
1456,1457,958,198,1456,1456,198,198,958,198,198,198,1457,958,958,1545,
958,958,958,1457,1457,1543,1543,1543,1546,1546,198,1546,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
    },
    { // bank 34, AIL (Bubble Bobble Feat. Rainbow Islands, Z)
1882,1883,1884,1885,1886,1887,1888,1889,1890,1891,1892,1893,1894,1895,1896,1897,
1898,1899,1900,1901,1902,1903,1904,1905,1906,1907,1908,1909,1910,1911,1912,1913,
1914,1915,1916,1917,1918,1919,1920,1921,1922,1923,1924,1925,1926,1927,1928,1929,
1930,1931,1932,1933,1934,1935,1936,1937,1938,1939,1940,1941,1942,1943,1944,1945,
1946,1947,1948,1949,1950,1951,1952,1953,1954,1955,1956,1957,1958,1959,1960,1961,
1962,1963,1964,1965,1966,1967,1968,1969,1970,1971,1972,1973,1974,1975,1976,1977,
1978,1979,1980,1981,1982,1983,1984,1985,1986,1987,1988,1989,1990,1991,1992,1993,
1994,1995,1996,1997,1998,1999,2000,2001,2002,2003,2004,2005,2006,2007,2008,2009,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,2010,2011,2011,2012,
2013,2014,2015,2016,127,2012,2017,2010,2017,131,2018,131,2018,131,2019,131,
131,2020,131,2021,2020,2021,2022,2020,2023,2020,2024,2021,2025,2026,2025,2027,
2027,2028,2028,2029,2029,152,2030,2031,2032,2033,2034,2035,2014,2014,2036,2037,
2038,2039,165,2039,2039,2012,2012,2040,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
    },
    { // bank 35, AIL (Warcraft)
1768,882,2041,884,2042,883,2043,2044,2045,198,198,198,198,198,2046,198,
198,198,198,198,198,198,198,198,198,198,2047,2048,2049,198,198,198,
2050,2050,896,2051,2052,2053,2054,2055,2056,1083,2057,2058,2059,2060,915,2061,
2062,2063,2064,2065,2066,2067,2068,2069,2070,2071,1794,2072,934,407,937,408,
198,2073,2074,2075,1741,1096,1098,1516,1751,1101,2076,2077,2078,198,1744,198,
 79,198,198,2079,2080,198,198,198,2081,198,198,198, 91,2082,2083, 94,
198,198,198,198,198,2084,198,198,198,198,198,198,198,198,2085,198,
198,198,198,198,1614,198,198,198,198,198,2086,198,198,198,198,198,
198,198,198,198,198,198,198,2087,2087,2087,2087,2087,2087,2087,2087,2087,
2087,2087,2087,2087,2087,2087,2087,2087,2087,2087,2087,2087,2088,198,1755,198,
198,2089,198,198,2090,2089,2091,2092,2093,2094,2094,2094,2094,2094,2094,2094,
2094,2094,2094,2094,2094,2094,2095,198,2096,2097,198,2098,2099,2099,2087,2087,
2087,2100,2100,2096,2096,2101,2101,2102,2102,2103,198,2104,198,198,198,198,
198,198,198,198,198,198,2105,2105,2106,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
    },
    { // bank 36, AIL (Terra Nova Strike Force Centuri)
649,650,2107,2108,653,654,655,2109,2110,2111,180,660,2112,2113,2114,185,
2115,2116,2117,668,2118,2119,671,672,673,674,675,2120,677,2121,2122,2123,
2124,2125,2126,2127,2128,2129,2130,2131,689,2132,2133,692,2134,2135,695,2136,
2137,2138,699, 50,2139,2140,703,2141,2142,2143,2144,2145,2146,2147, 59,2148,
2149,2150,2151,716,2152,2153,2154,2155,2156,2157,723,2158,725,2159,727,728,
2160,2161,2162,732,733,2163,2164,2165,737,2166,2167,740,2168,2169,2170,744,
2171,2172,747,2173,749,2174,2175,752,753,754,2176,2177,757,758,759,760,
761,762,763,2178,2179,2180,767,2181,2182,2183,2184,2185,2186,2187,2188,2189,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,2190,2191,2192,2193,2194,2195,2196,2197,2196,2198,2199,2200,2199,
2199,2201,2199,2202,2203,2204,2205,2206,141,2207,2208,2209,143,144,145,146,
147,148,149,2210,2211,2212,2213,154,155,2214,2215,2216,159,160,2217,881,
2218,2219,2220,166,2221,168,2222,2223,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
    },
    { // bank 37, AIL (System Shock)
2224,678,2225,2226,653,654,655,2227,2228,2229,659,660,661,662,663,664,
665,666,2230,668,669,670,671,672,2229,674,675,2231,2232,2224,2233,2234,
2228,2235,2236,684,685,2237,2238,682,689,690,691,692,693,694,695,2239,
697,698,2240,700,701,702,703,704,705,706,707,708,709,710,711,712,
713,714,715,716,717,718,719,720,721,722,723,2241,725,726,727,2242,
729,730,731,732,733,2243,2244,2245,2246,738,739,740,741,742,743,2247,
745,746,747,2248,749,750,751,752,753,754,755,2249,757,108,759,2227,
2250,2251,763,764,765,766,767,2252,2253,2254,771,772,2255,774,2256,776,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,2257,2257,128,129,130,2258,131,2259,131,133,131,134,131,
131,135,131,136,137,138,139,877,141,135,878,136,143,144,145,146,
147,148,149,150,151,152,153,154,155,879,880,158,159,160,161,881,
163,164,165,166,167,168,169,131,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
    },
    { // bank 38, AIL (Advanced Civilization)
1127,1128,1129,1130,1131,1132,1133,1134,1135,1136,1137,1138,1139,1140,1141,1142,
1143,1144,1145,1146,1147,1148,1149,1150,1151,1152,1153,1154,1155,1156,1157,1158,
1159,1160,1161,1162,1163,1164,1165,1166,2260,1168,1169,1170,1171,1172,1173,1174,
1175,1176,1177,1178,1179,1180,1181,1182,1183,1184,1185,1186,1187,1188,1189,1190,
1191,1192,1193,1194,1195,1196,1197,1198,1199,1200,1201,1202,1203,1204,2260,1206,
1207,1208,1209,1210,1211,1212,1213,1214,1215,1216,1217,1218,1219,1220,1221,1222,
1223,1224,1225,1226,1227,1228,1229,1230,1231,1232,1233,1234,1235,1236,1237,1238,
1239,1240,1241,1242,1243,1244,1245,1246,1247,1248,1249,1250,1251,1252,1253,1254,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,1255,1255,1256,1257,1258,1259,1260,1261,1260,1262,1260,1263,1260,
1260,1264,1260,1265,1266,1265,1267,1268,1269,1264,1270,1265,1271,1272,1273,1274,
1274,1275,1275,1276,1276,1277,1278,1279,1280,1281,1282,1283,1284,1284,1285,1286,
1287,1288,1289,1290,1290,1291,1292,1293,1294,1295,1296,1297,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
    },
    { // bank 39, AIL (Battle Chess 4000)
2261,2262,2263,2264,2265,2266,2267,2268,2269,2270,2271,2272,2273,2274,2275,2276,
2277,2278,2279,2280,2281,2282,2283,2284,2285,2286,2287,2288,2289,2290,2291,2292,
2293,2294,2295,2296,2297,2298,2299,2300,2301,2302,2303,2304,2305,2306,2307,2308,
2309,2310,2311,2309,2312,2313,2314,2315,2316,2317,2318,2319,2320,2321,2322,248,
2323,2324,2325,2326,2327,2328,2329,2330,2331,1743,2332,2333,2334,1103,2335,2336,
2337,2338,1107,2339,1864,2340,2341,2342,2343,407,2344,2345,2346,934,2347,2348,
2349,2350,2351,2352,2353,2354,2355,2356,2357,2358,1106,2359,2360,2361,2362,2363,
2364,2365,948,949,2366,1533,2061,2367,2368,2369,1060,2370,2371,898,2372,2373,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
    },
    { // bank 40, AIL (Ultimate Soccer Manager)
1047,1048,1049,1050,1051,1052,1053,893,1054,1055,1056,1057,1058,1059,1060,1061,
1062,1063,1064,1065,1066,1067,1068,1069,1070,1071,1072,918,1073,1074,1074,1074,
1075,1076,1077,1078,1079,2323,1080,1081,1082,1401,1084,198,1074,1085,1402,1087,
1403,1089,1074,1074,1074,1074,1074,1074,1404,1091,1405,1093,1406,408,407,937,
1095,1074,1074,1074,1096,1097,1098,1099,1100,1101,1102,2076,198,1103,198,198,
1739,2374,198,198,1104,198,198,2375,198,198,2375,198,198,198,198,198,
198,198,198,2266,198,198,198,198,198,198,1105,1106,198,198,198,1107,
198,198,198,1108,1087,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,1109,
198,1110,198,1111,1109,1112,1113,1114,1115,1116,1117,1116,1118,1116,1118,1116,
1116,1119,1116,1120,198,198,1118,198,1121,198,198,198,1109,1109,1109,1109,
1109,1109,1109,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
    },
    { // bank 41, HMI (Theme Park)
2376,883,1413,1414,1415,883,886,2377,1416,2378,1417,1464,1419,1465,1414,1421,
1466,1467,892,1422,893,1468,1469,1470,1424,1471,1426,1874,2379,896,896,896,
1427,2380,2381,1538,2382,352,1429,400,903,1430,1741,1484,896,896,1742,1875,
2401,1490,2383,1434,1653,2401,1653,1495,914,915,916,917,917,1877,1497,891,
921,896,896,2407,921,921,905,1435,1436,1436,1436,1436,2413,1510,1437,924,
924,1439,1440,1516,1517,1517,1518,1441,2385,2386,1747,1748,934,1529,1433,1521,
1443,1444,2387,941,1525,1526,1526,1527,941,1434,1528,1529,1529,1529,1530,1530,
946,1447,2388,1448,949,948,948,1533,1534,1535,1536,2389,1449,1450,2381,2381,
2390,2390,2390,2390,2390,2390,2390,2390,2390,2390,2390,2390,2390,2390,2390,2390,
2390,2390,2390,2390,2390,2390,2390,2390,2390,2390,2390,2390,2390,2390,2390,2390,
2390,2390,2390,198,2391,2392,2393,2394,2395,2396,2397,2396,2398,2399,2400,2398,
198,2402,2395,2390,2403,198,2390,2390,2399,2404,2390,2390,2405,2396,2395,2406,
2399,2395,2396,198,2408,2398,2398,2409,2410,2411,2390,2412,198,2390,2390,2390,
2390,2390,2390,2390,2390,2390,2390,2390,2390,2390,2390,2390,2390,2390,2390,2390,
2390,2390,2390,2390,2390,2390,2390,2390,2390,2390,2390,2390,2390,2390,2390,2390,
2390,2390,2390,2390,2390,2390,2390,2390,2390,2390,2390,2390,2390,2390,2390,2390,
    },
    { // bank 42, HMI (3d Table Sports, Battle Arena Toshinden)
1882,1883,1884,1885,1886,1887,1888,1889,1890,2462,1892,1893,1894,1895,1896,1897,
1898,1899,1900,1901,1902,1903,1904,1905,1906,1907,1908,1909,1910,1911,1912,1913,
1914,1915,1916,1917,1918,2463,1920,1921,1922,1923,1924,1925,1926,1927,1928,1929,
1930,1931,1932,1933,1934,1935,1936,1937,1938,1939,1940,1941,1942,1943,1944,1945,
1946,1947,1948,1949,1950,1951,1952,1953,1954,1955,1956,1957,1958,1959,1960,1961,
1962,1963,1964,1965,1966,1967,1968,1969,1970,1971,1972,1973,1974,1975,1976,1977,
1978,1979,1980,1981,1982,1983,1984,1985,1986,1987,1988,1989,1990,1991,1992,1993,
1994,2464,1996,1997,1998,1999,2000,2465,2466,2467,2468,2469,2470,2471,2472,198,
189,189,189,189,189,189,189,189,189,189,189,189,189,189,189,189,
189,189,189,189,189,189,189,189,189,189,189,189,2414,2415,2416,2417,
2418,2419,2420,2421,2422,2417,2423,2424,2425,2426,2427,2428,2427,2429,2430,2431,
2432,2433,2434,2435,2433,2435,2436,2433,2437,2433,216,2438,2439,2440,2441,2442,
2443,2444,2445,2446,2447,2448,2449,2450,2451,2452,2453,2454,2455,2419,2456,2457,
2458,239,2448,240,198,2460,2417,2461,189,189,189,189,189,189,189,189,
189,189,189,189,189,189,189,189,189,189,189,189,189,189,189,189,
189,189,189,189,189,189,189,189,189,189,189,189,189,189,189,198,
    },
    { // bank 43, HMI (Aces of the Deep)
2481,2482,2483,2484,2485,2486,2487,2488,2489,2490,2491,2492,2493,2494,2495,2496,
2497,2498,2499,2500,2501,2502,2503,2504,2505,2506,2507,2508,2509,2510,2511,2512,
2513,2514,2515,2516,2517,2518,2519,2520,2521,2522,2523,2524,2525,2526,2527,2528,
2529,2530,2531,2532,2533,2534,2535,2536,2537,2538,2539,2540,2541,2542,2543,2544,
2545,2546,2547,2548,2549,2550,2551,2552,2553,2554,2555,2556,2557,2558,2559,2560,
2561,2562,2563,2564,2565,2566,2567,2568,2569,2570,2571,2572,2573,2574,2575,2576,
2577,2578,2579,2580,2581,2582,2583,2584,2585,2586,2587,2588,2589,2590,2591,2592,
2593,2594,2595,2596,2597,2598,2599,2600,2601,2602,2603,2604,2605,2606,2607,2608,
189,189,189,189,189,189,189,189,189,189,189,189,189,189,189,189,
189,189,189,189,189,189,189,189,189,189,189,189,2473,2474,2475,200,
2476,235,2477,2478,199,200,201,202,203,204,2479,206,2479,207,2480,209,
210,211,212,213,211,213,214,211,215,211,216,217,218,219,220,221,
222,223,224,225,226,227,228,229,230,231,232,233,234,235,236,237,
238,239,227,240,241,242,200,243,189,189,189,189,189,189,189,189,
189,189,189,189,189,189,189,189,189,189,189,189,189,189,189,189,
189,189,189,189,189,189,189,189,189,189,189,189,189,189,189,189,
    },
    { // bank 44, HMI (Earthsiege)
2612,2482,2483,2613,2614,2615,2487,2616,2489,2490,2491,2492,2493,2617,2618,2619,
2620,2498,2621,2500,2501,2622,2503,2504,2505,2623,2507,2624,2625,2626,2627,2512,
2628,2629,2630,2631,2632,2633,2634,2635,2521,2636,2523,2524,2525,2637,2527,2638,
2639,2530,2531,2640,2533,2534,2535,2536,2641,2642,2643,2540,2644,2645,2646,2544,
2545,2546,2547,2548,2647,2550,2648,2649,2553,2650,2555,2556,2651,2652,1960,2653,
2654,2655,2563,2564,2656,2566,2657,2658,2569,2659,2660,2661,2662,2663,2664,2665,
2577,2666,2667,2668,2669,2582,2670,2671,2585,2672,2673,2588,2589,2590,2591,2674,
2593,2675,2676,2596,2597,2677,2678,2679,2601,2680,2681,2604,2605,2682,2683,2684,
189,189,189,189,189,189,189,189,189,189,189,189,189,189,189,189,
189,189,189,189,189,189,189,189,189,189,189,190,191,192,193,194,
195,196,197,198,2609,200,201,202,203,204,205,206,205,207,208,209,
210,211,212,213,211,213,214,211,215,211,216,217,218,219,220,221,
222,223,224,2610,2611,227,228,229,230,231,232,233,234,235,236,237,
238,239,227,240,241,242,200,243,189,189,189,189,189,189,189,189,
189,189,189,189,189,189,189,189,189,189,189,189,189,189,189,189,
189,189,189,189,189,189,189,189,189,189,189,189,189,189,189,189,
    },
    { // bank 45, HMI (Anvil of Dawn)
  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15,
 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
 32, 33, 34, 35, 36, 37, 38, 33, 39, 40, 41, 42, 43, 44, 45, 46,
 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62,
 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78,
 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94,
 95, 96, 97, 98, 99,100,101,102,103,104,105,106,107,108,109,110,
111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,
377,377,377,377,377,377,377,377,377,377,377,377,377,377,377,377,
377,377,377,377,377,377,377,377,377,377,377,377,377,377,377,377,
377,377,377,377,377,2685,2686,2687,2688,367,2689,368,2690,369,2691,370,
371,2692,372,2693,385,2694,2695,2696,2697,2698,2699,2700,2701,2702,2703,2704,
2705,374,2706,2707,2708,2709,2710,2711,2712,2713,2714,2715,2716,2717,2718,2719,
2720,2721,2722,2723,2724,2725,2726,429,429,429,429,429,429,429,429,429,
429,429,429,429,429,429,429,429,429,429,429,429,429,429,429,429,
429,429,429,429,429,429,429,429,429,429,429,429,429,429,429,429,
    },
    { // bank 46, AIL (Master of Magic, Master of Orion 2 :: std perccussion)
649,650,651,2731,653,654,655,2732,657,658,659,2733,661,662,663,664,
665,666,2729,668,669,670,671,672,673,674,675,676,677,678,679,680,
681,682,683,684,685,686,687,688,689,690,691,692,693,694,695,696,
697,698,699,700,701,702,703,704,705,706,2730,708,709,710,711,712,
713,714,715,716,717,718,719,720,721,722,723,724,725,726,727,728,
729,730,731,732,733,734,735,736,737,738,739,740,741,742,743,744,
745,746,747,748,749,750,751,752,753,754,755,756,757,758,759,760,
761,762,763,764,765,766,767,768,769,770,771,772,2734,2735,775,776,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,377,2737,2752,2756,2776,2753,2751,2754,2737,2755,2737,2757,2737,
2737,2777,2737,2738,2778,2739,2758,2759,2728,2779,2766,2740,2760,2761,2747,2749,
2767,2762,2763,2764,2768,2745,2769,2765,2727,2773,2774,2742,2743,2743,2775,2770,
2771,2772,2746,2736,2741,2744,2748,2750,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
    },
    { // bank 47, AIL (Master of Magic, Master of Orion 2 :: orchestral percussion)
649,650,651,2731,653,654,655,2732,657,658,659,2733,661,662,663,664,
665,666,2729,668,669,670,671,672,673,674,675,676,677,678,679,680,
681,682,683,684,685,686,687,688,689,690,691,692,693,694,695,696,
697,698,699,700,701,702,703,704,705,706,2730,708,709,710,711,712,
713,714,715,716,717,718,719,720,721,722,723,724,725,726,727,728,
729,730,731,732,733,734,735,736,737,738,739,740,741,742,743,744,
745,746,747,748,749,750,751,752,753,754,755,756,757,758,759,760,
761,762,763,764,765,766,767,768,769,770,771,772,2734,2735,775,776,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,2782,2782,2785,2783,2784,2783,2780,2780,2780,2780,2780,2780,2780,
2780,2780,2780,2780,2780,2780,2786,2787,2810,2777,2788,2777,2789,2790,2791,2792,
2793,2794,2795,2796,2797,2798,2799,2800,2801,2802,2803,2784,2784,2784,2804,2805,
2806,2807,2798,2808,2809,2784,2791,2792,2781,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
    },
};
