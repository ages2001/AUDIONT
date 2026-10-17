/*
 * AUDIONT - General MIDI synthesizer
 *
 * 32-voice, 2-operator FM synthesizer with a full GM instrument set and a
 * synthesized drum kit (pitched tone + filtered noise + metallic ring
 * modulation).  Integer arithmetic only.  Rendered at 48 kHz straight into
 * the software mixer, so MIDI and wave audio play at the same time.
 *
 * MIDI events are time-stamped on arrival and applied with a constant
 * latency (the mixer latency), giving sub-millisecond timing jitter.
 * Supports program/bank change, volume, expression, pan, modulation,
 * sustain, pitch bend with RPN bend range, all-notes/sound-off, reset
 * controllers, GM/GS/XG reset, GS drum part assignment, master volume.
 */
#include "audiont.h"
#include "synthtab.h"
#include "gmpatch.h"

#ifdef __x86_64__
#define USER_LIMIT 0x7FFFFFF0000ULL
#else
#define USER_LIMIT 0x7FFF0000UL
#endif

#define NVOICES     32
#define NEVENTS     1024
#define ENV_ONE     (1UL << 30)
#define ENV_OFF     (1UL << 16)          /* about -84 dB */

enum { ST_ATTACK, ST_DECAY, ST_SUSTAIN, ST_RELEASE, ST_OFF };

typedef struct _VOICE {
    UCHAR  Active, Chan, Note, Vel, Drum, Released, Sustained, Layer;
    ULONG  Age;
    const PATCH *P;
    const DRUM  *D;
    ULONG  PhM, PhC, Ph2;
    ULONG  EnvM, EnvC;
    UCHAR  StM, StC;
    UCHAR  RateDM, RateDC;              /* key-scaled decay rates */
    LONG   PrevEnvM, PrevEnvC;          /* Q15 at previous block end */
    LONG   ModPrev;
    LONG   VelGainM, VelGainC;          /* Q15 */
    LONG   DetCents64;                  /* layer detune, 1/64 semitone */
    /* drum */
    ULONG  Inc, IncTarget;
    ULONG  EnvT, EnvN;
    LONG   PrevEnvT, PrevEnvN;
    ULONG  Noise;
    LONG   Lp, Lp2;
} VOICE;

typedef struct _CHANNEL {
    UCHAR Program, BankMsb, BankLsb, Volume, Expression, Pan, Mod, Sustain, Drum;
    UCHAR RpnMsb, RpnLsb, BendRange, BendCents;
    LONG  Bend;                         /* -8192..8191 */
    UCHAR Pressure;
} CHANNEL;

typedef struct _MEVENT { ULONG Frame; UCHAR St, D1, D2, Pad; } MEVENT;

typedef struct _SYNTH {
    VOICE   V[NVOICES];
    CHANNEL Ch[16];
    MEVENT  Ev[NEVENTS];
    ULONG   EvHead, EvTail;
    ULONG   AgeCounter;
    ULONG   Lfo, LfoFast, LfoSlow;      /* LFO phases (2^32 per cycle) */
    LONG    Buf[SYN_BLOCK * 2];
    ULONG   BufPos;
    ULONG   BlockFrame;                 /* engine frame of the next block */
    /* parser */
    UCHAR   Status, Need, Have, Data[2];
    UCHAR   Sysex[64];
    ULONG   SysexLen;
    BOOLEAN InSysex;
    LONG    MasterVol;                  /* Q15 */
} SYNTH;

/* ------------------------------------------------------------------ */
static __inline__ ULONG Mul30(ULONG a, ULONG b) { return (ULONG)(((ULONGLONG)a * b) >> 30); }

static ULONG PitchInc(ULONG note, LONG off64)
{
    LONG total = (LONG)note * 64 + off64, rem;
    ULONGLONG inc;
    if (total < 0) total = 0;
    if (total > 127 * 64) total = 127 * 64;
    note = total / 64; rem = total % 64;              /* 1/64 semitone = 1/768 octave */
    inc = ((ULONGLONG)g_NoteInc[note] * g_Pow2[rem]) >> 16;
    if (inc > 0x7FFFFFFFULL) inc = 0x7FFFFFFFULL;
    return (ULONG)inc;
}

static __inline__ ULONG MultInc(ULONG inc, UCHAR mult)
{
    ULONGLONG v = ((ULONGLONG)inc * (mult ? mult : 1)) >> 1;
    return (ULONG)v;
}

static UCHAR KeyRate(UCHAR rate, UCHAR ks, UCHAR note)
{
    LONG r = rate - (LONG)ks * ((LONG)note - 60) / 6;
    if (r < 0) r = 0;
    if (r > 127) r = 127;
    return (UCHAR)r;
}

static void EnvStep(ULONG *env, UCHAR *st, const FMOP *op, UCHAR decayRate)
{
    ULONG target;
    switch (*st) {
    case ST_ATTACK:
        *env += g_AttackInc[op->A];
        if (*env >= ENV_ONE || op->A == 0) { *env = ENV_ONE; *st = ST_DECAY; }
        break;
    case ST_DECAY:
        target = (ULONG)op->S * (ENV_ONE / 127);
        if (op->S >= 127) { *env = ENV_ONE; *st = ST_SUSTAIN; break; }
        *env = target + Mul30(*env - target, g_DecayMul[decayRate]);
        if (*env - target < ENV_OFF) { *env = target; *st = ST_SUSTAIN; }
        break;
    case ST_SUSTAIN:
        break;
    case ST_RELEASE:
        *env = Mul30(*env, g_DecayMul[op->R]);
        if (*env < ENV_OFF) { *env = 0; *st = ST_OFF; }
        break;
    default:
        *env = 0;
    }
}

/* ------------------------------------------------------------------ */
/* voice management                                                    */
/* ------------------------------------------------------------------ */
static VOICE *AllocVoice(SYNTH *s, UCHAR chan, UCHAR note)
{
    ULONG i, best = 0, bestScore = 0;
    for (i = 0; i < NVOICES; i++) if (!s->V[i].Active) return &s->V[i];
    /* steal: prefer released voices, then the oldest */
    for (i = 0; i < NVOICES; i++) {
        VOICE *v = &s->V[i];
        ULONG score = (s->AgeCounter - v->Age) + (v->Released ? 0x40000000UL : 0) +
                      (v->Chan == chan && v->Note == note ? 0x80000000UL : 0);
        if (score >= bestScore) { bestScore = score; best = i; }
    }
    return &s->V[best];
}

static void NoteOff(SYNTH *s, UCHAR ch, UCHAR note)
{
    ULONG i;
    for (i = 0; i < NVOICES; i++) {
        VOICE *v = &s->V[i];
        if (!v->Active || v->Chan != ch || v->Note != note || v->Released || v->Drum) continue;
        if (s->Ch[ch].Sustain >= 64) { v->Sustained = TRUE; continue; }
        v->Released = TRUE;
        v->StM = ST_RELEASE; v->StC = ST_RELEASE;
    }
}

static void ReleaseSustained(SYNTH *s, UCHAR ch)
{
    ULONG i;
    for (i = 0; i < NVOICES; i++) {
        VOICE *v = &s->V[i];
        if (v->Active && v->Chan == ch && v->Sustained && !v->Released) {
            v->Sustained = FALSE; v->Released = TRUE;
            v->StM = ST_RELEASE; v->StC = ST_RELEASE;
        }
    }
}

static void AllNotesOff(SYNTH *s, UCHAR ch, BOOLEAN kill)
{
    ULONG i;
    for (i = 0; i < NVOICES; i++) {
        VOICE *v = &s->V[i];
        if (!v->Active || v->Chan != ch) continue;
        if (kill) v->Active = FALSE;
        else if (!v->Drum) { v->Released = TRUE; v->Sustained = FALSE; v->StM = ST_RELEASE; v->StC = ST_RELEASE; }
    }
}

static void StartMelodic(SYNTH *s, VOICE *v, UCHAR ch, UCHAR note, UCHAR vel, const PATCH *p, LONG det)
{
    LONG vs;
    memset(v, 0, sizeof(*v));
    v->Active = TRUE; v->Chan = ch; v->Note = note; v->Vel = vel; v->P = p;
    v->Age = ++s->AgeCounter;
    v->DetCents64 = det;
    v->StM = ST_ATTACK; v->StC = ST_ATTACK;
    v->RateDM = KeyRate(p->Mod.D, p->Mod.KeyScale, note);
    v->RateDC = KeyRate(p->Car.D, p->Car.KeyScale, note);
    if (p->Mod.A == 0) { v->EnvM = ENV_ONE; v->StM = ST_DECAY; }
    if (p->Car.A == 0) { v->EnvC = ENV_ONE; v->StC = ST_DECAY; }
    v->PrevEnvM = v->EnvM >> 15; v->PrevEnvC = 0;
    vs = p->Mod.VelSens;
    v->VelGainM = 32767 - (vs * (32767 - g_VelCurve[vel])) / 127;
    vs = p->Car.VelSens;
    v->VelGainC = 32767 - (vs * (32767 - g_VelCurve[vel])) / 127;
    v->Noise = 0x12345678UL ^ (note * 2654435761UL);
}

static void StartDrum(SYNTH *s, VOICE *v, UCHAR ch, UCHAR note, UCHAR vel)
{
    const DRUM *d;
    ULONG i;
    if (note < DRUM_FIRST || note >= DRUM_FIRST + ARRAYSIZE(g_Drum)) return;
    d = &g_Drum[note - DRUM_FIRST];
    /* hi-hat choke */
    if (note == 42 || note == 44)
        for (i = 0; i < NVOICES; i++)
            if (s->V[i].Active && s->V[i].Drum && s->V[i].Note == 46) s->V[i].Active = FALSE;
    memset(v, 0, sizeof(*v));
    v->Active = TRUE; v->Drum = TRUE; v->Chan = ch; v->Note = note; v->Vel = vel; v->D = d;
    v->Age = ++s->AgeCounter;
    v->IncTarget = (ULONG)(((ULONGLONG)d->ToneHz << 32) / ENG_RATE);
    v->Inc = v->IncTarget;
    if (d->Sweep) {
        ULONG k = d->Sweep * 48;                           /* 1/16 octave -> 1/768 octave */
        ULONGLONG x = (ULONGLONG)v->IncTarget << (k / 768);
        x = (x * g_Pow2[k % 768]) >> 16;
        v->Inc = x > 0x7FFFFFFFULL ? 0x7FFFFFFFUL : (ULONG)x;
    }
    v->EnvT = d->ToneLvl ? ENV_ONE : 0;
    v->EnvN = d->NoiseLvl ? ENV_ONE : 0;
    v->PrevEnvT = v->EnvT >> 15; v->PrevEnvN = v->EnvN >> 15;
    v->VelGainC = g_VelCurve[vel];
    v->Noise = 0x9E3779B9UL ^ (note * 40503UL) ^ s->AgeCounter;
}

static void NoteOn(SYNTH *s, UCHAR ch, UCHAR note, UCHAR vel)
{
    CHANNEL *c = &s->Ch[ch];
    const PATCH *p;
    VOICE *v;
    LONG n;
    if (!vel) { NoteOff(s, ch, note); return; }
    if (c->Drum) { v = AllocVoice(s, ch, note); StartDrum(s, v, ch, note, vel); return; }
    p = &g_GmPatch[c->Program & 127];
    n = (LONG)note + p->Transpose;
    if (n < 0 || n > 127) return;
    /* retrigger of the same note on this channel releases the old voice */
    NoteOff(s, ch, note);
    v = AllocVoice(s, ch, note);
    StartMelodic(s, v, ch, note, vel, p, p->Layer ? -(LONG)p->Layer * 64 / 200 : 0);
    if (p->Layer) {
        v = AllocVoice(s, ch, note);
        StartMelodic(s, v, ch, note, vel, p, (LONG)p->Layer * 64 / 200);
        v->Layer = TRUE;
    }
}

static void ResetChannel(CHANNEL *c, ULONG idx)
{
    memset(c, 0, sizeof(*c));
    c->Volume = 100; c->Expression = 127; c->Pan = 64; c->BendRange = 2;
    c->RpnMsb = c->RpnLsb = 127;
    c->Drum = (idx == 9);
}

static void ResetControllers(CHANNEL *c)
{
    c->Expression = 127; c->Mod = 0; c->Sustain = 0; c->Bend = 0; c->Pressure = 0;
    c->RpnMsb = c->RpnLsb = 127;
}

static void FullReset(SYNTH *s)
{
    ULONG i;
    for (i = 0; i < NVOICES; i++) s->V[i].Active = FALSE;
    for (i = 0; i < 16; i++) ResetChannel(&s->Ch[i], i);
    s->MasterVol = 32767;
}

static void Controller(SYNTH *s, UCHAR ch, UCHAR cc, UCHAR val)
{
    CHANNEL *c = &s->Ch[ch];
    switch (cc) {
    case 0:  c->BankMsb = val; break;
    case 32: c->BankLsb = val; break;
    case 1:  c->Mod = val; break;
    case 7:  c->Volume = val; break;
    case 10: c->Pan = val; break;
    case 11: c->Expression = val; break;
    case 64: c->Sustain = val; if (val < 64) ReleaseSustained(s, ch); break;
    case 100: c->RpnLsb = val; break;
    case 101: c->RpnMsb = val; break;
    case 98: case 99: c->RpnMsb = c->RpnLsb = 127; break;      /* NRPN: deselect RPN */
    case 6:
        if (c->RpnMsb == 0 && c->RpnLsb == 0) c->BendRange = val > 24 ? 24 : val;
        break;
    case 38:
        if (c->RpnMsb == 0 && c->RpnLsb == 0) c->BendCents = val;
        break;
    case 120: AllNotesOff(s, ch, TRUE); break;
    case 121: ResetControllers(c); break;
    case 123: case 124: case 125: case 126: case 127: AllNotesOff(s, ch, FALSE); break;
    default: break;
    }
}

static void Dispatch(SYNTH *s, UCHAR st, UCHAR d1, UCHAR d2)
{
    UCHAR ch = st & 15;
    switch (st & 0xF0) {
    case 0x80: NoteOff(s, ch, d1); break;
    case 0x90: NoteOn(s, ch, d1, d2); break;
    case 0xB0: Controller(s, ch, d1, d2); break;
    case 0xC0: s->Ch[ch].Program = d1; break;
    case 0xD0: s->Ch[ch].Pressure = d1; break;
    case 0xE0: s->Ch[ch].Bend = (LONG)((d2 << 7) | d1) - 8192; break;
    case 0xF0:
        if (st == 0xFF) FullReset(s);
        break;
    }
}

static void Sysex(SYNTH *s, const UCHAR *m, ULONG n)
{
    /* m excludes F0/F7 */
    if (n >= 4 && m[0] == 0x7E && m[2] == 0x09 && (m[3] == 0x01 || m[3] == 0x03)) {
        FullReset(s); LOGV("synth: GM system on"); return;
    }
    if (n >= 9 && m[0] == 0x41 && m[2] == 0x42 && m[3] == 0x12 && m[4] == 0x40 && m[5] == 0x00 && m[6] == 0x7F) {
        FullReset(s); LOGV("synth: GS reset"); return;
    }
    if (n >= 7 && m[0] == 0x43 && (m[1] & 0xF0) == 0x10 && m[2] == 0x4C && m[3] == 0 && m[4] == 0 && m[5] == 0x7E) {
        FullReset(s); LOGV("synth: XG system on"); return;
    }
    if (n >= 6 && m[0] == 0x7F && m[2] == 0x04 && m[3] == 0x01) {
        s->MasterVol = (LONG)(((m[5] << 7) | m[4]) * 32767UL / 16383);
        return;
    }
    if (n >= 8 && m[0] == 0x41 && m[2] == 0x42 && m[3] == 0x12 && m[4] == 0x40 &&
        (m[5] & 0xF0) == 0x10 && m[6] == 0x15) {
        static const UCHAR part2ch[16] = { 9, 0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 12, 13, 14, 15 };
        s->Ch[part2ch[m[5] & 15]].Drum = m[7] ? 1 : 0;
    }
}

/* ------------------------------------------------------------------ */
/* rendering                                                           */
/* ------------------------------------------------------------------ */
static __inline__ LONG LfoSin(ULONG ph) { return g_Wave[0][ph >> 22]; }

static void RenderMelodic(SYNTH *s, VOICE *v, LONG *buf)
{
    const PATCH *p = v->P;
    CHANNEL *c = &s->Ch[v->Chan];
    const short *wm = g_Wave[p->Mod.Wave & 7], *wc = g_Wave[p->Car.Wave & 7];
    LONG off, envM0, envM1, envC0, envC1, dM, dC, gain, gl, gr, modDepth, fbDepth, addLvl, noiseLvl;
    ULONG incM, incC, i, lfo;
    LONG trem = 32767;

    /* pitch (1/64 semitone units) */
    lfo = (p->Flags & PF_FASTLFO) ? s->LfoFast : (p->Flags & PF_SLOWLFO) ? s->LfoSlow : s->Lfo;
    off = (c->Bend * ((LONG)c->BendRange * 64 + (LONG)c->BendCents * 64 / 100)) / 8192;
    off += v->DetCents64;
    off += (LfoSin(lfo) * (((LONG)p->Vibrato + c->Mod * 50 / 127) * 64 / 100)) >> 15;
    if (p->Tremolo) trem = 32767 - ((LONG)p->Tremolo * (32767 - LfoSin(lfo)) >> 8);
    incC = MultInc(PitchInc((ULONG)((LONG)v->Note + p->Transpose), off + p->Car.Detune * 64 / 100), p->Car.Mult);
    incM = MultInc(PitchInc((ULONG)((LONG)v->Note + p->Transpose), off + p->Mod.Detune * 64 / 100), p->Mod.Mult);

    /* envelopes for this block */
    envM0 = v->PrevEnvM; envC0 = v->PrevEnvC;
    EnvStep(&v->EnvM, &v->StM, &p->Mod, v->RateDM);
    EnvStep(&v->EnvC, &v->StC, &p->Car, v->RateDC);
    envM1 = v->EnvM >> 15; envC1 = v->EnvC >> 15;
    v->PrevEnvM = envM1; v->PrevEnvC = envC1;
    dM = (envM1 - envM0) / SYN_BLOCK; dC = (envC1 - envC0) / SYN_BLOCK;

    /* gains */
    gain = (LONG)(((ULONG)g_VolCurve[c->Volume] * g_VolCurve[c->Expression]) >> 15);
    gain = (gain * v->VelGainC) >> 15;
    gain = (gain * ((LONG)p->Car.Level * 258)) >> 15;          /* level 127 ~ 1.0 */
    gain = (gain * trem) >> 15;
    gain = (gain * s->MasterVol) >> 15;
    gl = (gain * g_PanTab[c->Pan][0]) >> 15;
    gr = (gain * g_PanTab[c->Pan][1]) >> 15;
    modDepth = (LONG)(((LONG)p->Mod.Level * 1314 * v->VelGainM) >> 15);
    fbDepth = (LONG)p->Feedback * 9000;
    addLvl = (LONG)p->Mod.Level * 258;
    noiseLvl = (LONG)p->Noise * 200;

    for (i = 0; i < SYN_BLOCK; i++) {
        LONG m, o;
        ULONG ph;
        envM0 += dM; envC0 += dC;
        ph = v->PhM + (ULONG)(v->ModPrev * fbDepth);
        m = (wm[ph >> 22] * envM0) >> 15;
        v->ModPrev = m;
        if (p->Alg == 0) o = wc[(v->PhC + (ULONG)m * (ULONG)modDepth) >> 22];
        else o = wc[v->PhC >> 22] + ((m * addLvl) >> 15);
        if (noiseLvl) {
            LONG nz;
            v->Noise = v->Noise * 1664525UL + 1013904223UL;
            nz = (LONG)(v->Noise >> 16) - 32768;
            v->Lp += (nz - v->Lp) >> 2;
            o = ((o * (32767 - noiseLvl / 2)) >> 15) + ((v->Lp * noiseLvl) >> 15);
        }
        if (o > 32767) o = 32767; else if (o < -32767) o = -32767;
        o = (o * envC0) >> 15;
        buf[i * 2]     += (o * gl) >> 16;
        buf[i * 2 + 1] += (o * gr) >> 16;
        v->PhM += incM;
        v->PhC += incC;
    }
    if (v->StC == ST_OFF || (v->StC == ST_SUSTAIN && v->EnvC == 0)) v->Active = FALSE;
}

static void RenderDrum(SYNTH *s, VOICE *v, LONG *buf)
{
    const DRUM *d = v->D;
    CHANNEL *c = &s->Ch[v->Chan];
    LONG t0 = v->PrevEnvT, n0 = v->PrevEnvN, t1, n1, dt, dn, gain, gl, gr, tl, nl;
    ULONG i, inc2;
    v->EnvT = Mul30(v->EnvT, g_DecayMul[d->ToneDecay]);
    v->EnvN = Mul30(v->EnvN, g_DecayMul[d->NoiseDecay]);
    t1 = v->EnvT >> 15; n1 = v->EnvN >> 15;
    v->PrevEnvT = t1; v->PrevEnvN = n1;
    dt = (t1 - t0) / SYN_BLOCK; dn = (n1 - n0) / SYN_BLOCK;
    if (d->Sweep) v->Inc = v->IncTarget + Mul30(v->Inc - v->IncTarget, g_DecayMul[d->SweepRate]);
    inc2 = (ULONG)(((ULONGLONG)v->Inc * 47089) >> 15);          /* x1.437: metallic partner */
    gain = (LONG)(((ULONG)g_VolCurve[c->Volume] * g_VolCurve[c->Expression]) >> 15);
    gain = (gain * v->VelGainC) >> 15;
    gain = (gain * s->MasterVol) >> 15;
    gl = (gain * g_PanTab[d->Pan][0]) >> 15;
    gr = (gain * g_PanTab[d->Pan][1]) >> 15;
    tl = (LONG)d->ToneLvl * 258;
    nl = (LONG)d->NoiseLvl * 258;
    for (i = 0; i < SYN_BLOCK; i++) {
        LONG tone, nz, o;
        t0 += dt; n0 += dn;
        if (d->Metal) tone = (g_Wave[4][v->PhC >> 22] * g_Wave[4][v->Ph2 >> 22]) >> 15;
        else tone = g_Wave[0][v->PhC >> 22];
        v->PhC += v->Inc; v->Ph2 += inc2;
        v->Noise = v->Noise * 1664525UL + 1013904223UL;
        nz = (LONG)(v->Noise >> 16) - 32768;
        switch (d->Color) {
        case 0: v->Lp += (nz - v->Lp) >> 3; nz = v->Lp; break;
        case 1: v->Lp += (nz - v->Lp) >> 1; v->Lp2 += (v->Lp - v->Lp2) >> 4; nz = v->Lp - v->Lp2; break;
        default: v->Lp += (nz - v->Lp) >> 1; nz = nz - v->Lp; break;
        }
        o = (((tone * t0) >> 15) * tl >> 15) + (((nz * n0) >> 15) * nl >> 15);
        buf[i * 2]     += (o * gl) >> 16;
        buf[i * 2 + 1] += (o * gr) >> 16;
    }
    if (v->EnvT < ENV_OFF && v->EnvN < ENV_OFF) v->Active = FALSE;
}

static void RenderBlock(CARD *c, SYNTH *s)
{
    ULONG i;
    /* apply due events */
    while (s->EvTail != s->EvHead) {
        MEVENT *e = &s->Ev[s->EvTail % NEVENTS];
        if ((LONG)(e->Frame - s->BlockFrame) > 0) break;
        Dispatch(s, e->St, e->D1, e->D2);
        s->EvTail++;
    }
    memset(s->Buf, 0, sizeof(s->Buf));
    for (i = 0; i < NVOICES; i++) {
        VOICE *v = &s->V[i];
        if (!v->Active) continue;
        if (v->Drum) RenderDrum(s, v, s->Buf);
        else RenderMelodic(s, v, s->Buf);
    }
    s->Lfo += 15748214UL;          /* 5.5 Hz at 48 kHz / 32-frame blocks */
    s->LfoFast += 37222383UL;      /* 13 Hz */
    s->LfoSlow += 1145325UL;       /* 0.4 Hz */
    s->BlockFrame += SYN_BLOCK;
    s->BufPos = 0;
    (void)c;
}

void SynthRender(CARD *c, LONG *mix, ULONG frames)
{
    SYNTH *s = c->Synth;
    LONG gl = CtrlGain(c, LINE_SYNTH, 0), gr = CtrlGain(c, LINE_SYNTH, 1);
    ULONG i;
    if (!s || !SynthActive(c)) {
        if (s) s->BlockFrame = (ULONG)c->WrittenFrames + frames;
        return;
    }
    for (i = 0; i < frames; i++) {
        if (s->BufPos >= SYN_BLOCK) RenderBlock(c, s);
        mix[i * 2]     += (s->Buf[s->BufPos * 2] * gl) >> 15;
        mix[i * 2 + 1] += (s->Buf[s->BufPos * 2 + 1] * gr) >> 15;
        s->BufPos++;
    }
}

BOOLEAN SynthActive(CARD *c)
{
    SYNTH *s = c->Synth;
    ULONG i;
    if (!s) return FALSE;
    if (s->EvHead != s->EvTail) return TRUE;
    if (s->BufPos < SYN_BLOCK) {
        for (i = s->BufPos; i < SYN_BLOCK; i++)
            if (s->Buf[i * 2] || s->Buf[i * 2 + 1]) return TRUE;
    }
    for (i = 0; i < NVOICES; i++) if (s->V[i].Active) return TRUE;
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* MIDI byte stream parser (lock held)                                 */
/* ------------------------------------------------------------------ */
static void Queue(CARD *c, SYNTH *s, UCHAR st, UCHAR d1, UCHAR d2)
{
    MEVENT *e;
    ULONG when = (ULONG)(c->PlayedFrames + c->LatencyFrames);
    if ((LONG)(when - (ULONG)c->WrittenFrames) < 0) when = (ULONG)c->WrittenFrames;
    if (s->EvHead - s->EvTail >= NEVENTS) {          /* full: apply the oldest now */
        MEVENT *o = &s->Ev[s->EvTail % NEVENTS];
        Dispatch(s, o->St, o->D1, o->D2);
        s->EvTail++;
    }
    e = &s->Ev[s->EvHead % NEVENTS];
    e->Frame = when; e->St = st; e->D1 = d1; e->D2 = d2;
    s->EvHead++;
}

static UCHAR DataLen(UCHAR st)
{
    switch (st & 0xF0) {
    case 0xC0: case 0xD0: return 1;
    case 0xF0:
        if (st == 0xF1 || st == 0xF3) return 1;
        if (st == 0xF2) return 2;
        return 0;
    default: return 2;
    }
}

void SynthMidiBytes(CARD *c, const UCHAR *p, ULONG n)
{
    SYNTH *s = c->Synth;
    ULONG i;
    if (!s) return;
    if (!c->Playing) EngineKick(c);
    else EngineProcess(c);
    for (i = 0; i < n; i++) {
        UCHAR b = p[i];
        if (b >= 0xF8) {                                   /* real time */
            if (b == 0xFF) Queue(c, s, 0xFF, 0, 0);
            continue;
        }
        if (s->InSysex) {
            if (b == 0xF7 || (b & 0x80)) {
                Sysex(s, s->Sysex, s->SysexLen);
                s->InSysex = FALSE;
                if (b == 0xF7) continue;
            } else {
                if (s->SysexLen < sizeof(s->Sysex)) s->Sysex[s->SysexLen++] = b;
                continue;
            }
        }
        if (b & 0x80) {
            if (b == 0xF0) { s->InSysex = TRUE; s->SysexLen = 0; s->Status = 0; continue; }
            s->Status = b; s->Need = DataLen(b); s->Have = 0;
            if (b >= 0xF0) s->Status = 0;                   /* system common cancels running status */
            continue;
        }
        if (!s->Status) continue;
        s->Data[s->Have++] = b;
        if (s->Have >= s->Need) {
            Queue(c, s, s->Status, s->Data[0], s->Need > 1 ? s->Data[1] : 0);
            s->Have = 0;
        }
    }
    EngineKick(c);
}

void SynthReset(CARD *c)
{
    SYNTH *s = c->Synth;
    ULONG i;
    if (!s) return;
    s->EvTail = s->EvHead;
    for (i = 0; i < 16; i++) { AllNotesOff(s, (UCHAR)i, FALSE); ResetControllers(&s->Ch[i]); }
    s->Status = 0; s->InSysex = FALSE;
}

/* ------------------------------------------------------------------ */
NTSTATUS SynthCreate(CARD *c)
{
    SYNTH *s;
    if (!OsRegGetDword(L"Synth", 1)) return STATUS_SUCCESS;
    s = OsAlloc(sizeof(SYNTH));
    if (!s) return STATUS_INSUFFICIENT_RESOURCES;
    FullReset(s);
    s->BufPos = SYN_BLOCK;
    c->Synth = s;
    LOGV("synth: %u voices, %u bytes", NVOICES, sizeof(SYNTH));
    return STATUS_SUCCESS;
}

void SynthDestroy(CARD *c)
{
    OsFree(c->Synth);
    c->Synth = NULL;
}

NTSTATUS MidiOutIoctl(CARD *c, PIRP irp, PIO_STACK_LOCATION sp)
{
    PVOID buf = irp->AssociatedIrp.SystemBuffer;
    switch (IrpCode(sp)) {
    case IOCTL_MIDI_GET_CAPABILITIES: {
        MIDIOUTCAPSW caps; char name[40];
        memset(&caps, 0, sizeof(caps));
        caps.wMid = MM_MICROSOFT; caps.wPid = 3;
        caps.vDriverVersion = AUDIONT_VERSION;
        CardNameFor(c, DEV_MIDIOUT, name, sizeof(name));
        OsWcsCopyA(caps.szPname, name, MAXPNAMELEN);
        caps.wTechnology = MOD_SYNTH;
        if (!c->Synth) {
            /* USB MIDI port: an external instrument */
            caps.wTechnology = MOD_MIDIPORT;
            caps.wVoices = 0; caps.wNotes = 0;
            caps.dwSupport = 0;
            irp->IoStatus.Information = MIN(IrpOutLen(sp), sizeof(caps));
            memcpy(buf, &caps, irp->IoStatus.Information);
            return STATUS_SUCCESS;
        }
        caps.wVoices = NVOICES; caps.wNotes = NVOICES;
        caps.wChannelMask = 0xFFFF;
        caps.dwSupport = MIDICAPS_VOLUME | MIDICAPS_LRVOLUME | MIDICAPS_CACHE;
        irp->IoStatus.Information = MIN(IrpOutLen(sp), sizeof(caps));
        memcpy(buf, &caps, irp->IoStatus.Information);
        return STATUS_SUCCESS;
    }
    case IOCTL_MIDI_PLAY: {
        const UCHAR *src = sp->Parameters.DeviceIoControl.Type3InputBuffer;
        ULONG len = IrpInLen(sp), off = 0;
        UCHAR tmp[256];
        if (!src && len) return STATUS_INVALID_PARAMETER;
        if (irp->RequestorMode != 0 &&
            ((ULONG_PTR)src >= USER_LIMIT || (ULONG_PTR)src + len < (ULONG_PTR)src || (ULONG_PTR)src + len > USER_LIMIT))
            return STATUS_ACCESS_DENIED;
        while (off < len) {
            ULONG n = MIN(len - off, sizeof(tmp));
            memcpy(tmp, src + off, n);
            if (!c->Synth) { UsbMidiOut(c, tmp, n); off += n; continue; }
            EngineLock(c);
            SynthMidiBytes(c, tmp, n);
            EngineUnlock(c);
            off += n;
        }
        return STATUS_SUCCESS;
    }
    case IOCTL_MIDI_SET_STATE:
        if (IrpInLen(sp) < 4) return STATUS_INVALID_PARAMETER;
        if (*(PULONG)buf == MIDI_DD_RESET || *(PULONG)buf == MIDI_DD_STOP) {
            if (!c->Synth) UsbMidiReset(c);
            else { EngineLock(c); SynthReset(c); EngineUnlock(c); }
        }
        return STATUS_SUCCESS;
    case IOCTL_MIDI_SET_VOLUME:
        if (IrpInLen(sp) < sizeof(MIDI_DD_VOLUME)) return STATUS_INVALID_PARAMETER;
        c->Lines[LINE_SYNTH].Left = (USHORT)(((MIDI_DD_VOLUME *)buf)->Left >> 16);
        c->Lines[LINE_SYNTH].Right = (USHORT)(((MIDI_DD_VOLUME *)buf)->Right >> 16);
        c->ChangeCount++;
        return STATUS_SUCCESS;
    case IOCTL_MIDI_GET_VOLUME: {
        MIDI_DD_VOLUME v;
        if (IrpOutLen(sp) < sizeof(v)) return STATUS_BUFFER_TOO_SMALL;
        v.Left = (ULONG)c->Lines[LINE_SYNTH].Left << 16 | c->Lines[LINE_SYNTH].Left;
        v.Right = (ULONG)c->Lines[LINE_SYNTH].Right << 16 | c->Lines[LINE_SYNTH].Right;
        memcpy(buf, &v, sizeof(v));
        irp->IoStatus.Information = sizeof(v);
        return STATUS_SUCCESS;
    }
    case IOCTL_MIDI_CACHE_PATCHES:
    case IOCTL_MIDI_CACHE_DRUM_PATCHES:
        return STATUS_SUCCESS;
    default:
        return STATUS_NOT_SUPPORTED;
    }
}
