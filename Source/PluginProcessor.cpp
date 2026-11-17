#include "PluginProcessor.h"
#include <cmath>

using namespace juce;

// Dlugosci podzialow w cwiercnutach (1 = cwiercnuta)
static const double kDivBeats[] = { 1.0, 0.5, 0.25, 0.125, 1.0 / 3.0, 1.0 / 6.0 };

AudioProcessorValueTreeState::ParameterLayout FreeStutterProcessor::createLayout()
{
    std::vector<std::unique_ptr<RangedAudioParameter>> p;
    p.push_back (std::make_unique<AudioParameterBool>   (ParameterID { "hold", 1 },    "Stutter (Hold)", false));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "div", 1 },     "Division",
                 StringArray { "1/4", "1/8", "1/16", "1/32", "1/8 T", "1/16 T" }, 2));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "auto", 1 },    "Auto (koniec taktu)",
                 StringArray { "Off", "Last 1/4 bar", "Last 1/2 bar" }, 0));
    p.push_back (std::make_unique<AudioParameterFloat>  (ParameterID { "pitch", 1 },   "Pitch (semitones)",
                 NormalisableRange<float> (-12.0f, 12.0f, 0.01f), 0.0f));
    p.push_back (std::make_unique<AudioParameterBool>   (ParameterID { "reverse", 1 }, "Reverse", false));
    p.push_back (std::make_unique<AudioParameterFloat>  (ParameterID { "mix", 1 },     "Mix",
                 NormalisableRange<float> (0.0f, 1.0f, 0.001f), 1.0f));
    p.push_back (std::make_unique<AudioParameterFloat>  (ParameterID { "smooth", 1 },  "Fade (ms)",
                 NormalisableRange<float> (0.5f, 20.0f, 0.1f), 3.0f));
    return { p.begin(), p.end() };
}

FreeStutterProcessor::FreeStutterProcessor()
    : AudioProcessor (BusesProperties()
                        .withInput  ("Input",  AudioChannelSet::stereo(), true)
                        .withOutput ("Output", AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout())
{
}

bool FreeStutterProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    return l.getMainOutputChannelSet() == AudioChannelSet::stereo()
        && l.getMainInputChannelSet()  == AudioChannelSet::stereo();
}

void FreeStutterProcessor::prepareToPlay (double sampleRate, int)
{
    sr = sampleRate;
    ring.setSize (2, (int) (4.0 * sr) + 8);
    ring.clear();
    writePos = 0; frozen = false; act = 0.0f; phase = 0.0;
}

void FreeStutterProcessor::processBlock (AudioBuffer<float>& buffer, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    const int numCh = jmin (buffer.getNumChannels(), 2);
    const int ringSize = ring.getNumSamples();

    double bpm = 120.0, ppq = 0.0;
    bool havePpq = false;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            if (auto b = pos->getBpm()) bpm = *b;
            if (auto q = pos->getPpqPosition()) { ppq = *q; havePpq = true; }
        }
    if (bpm < 20.0) bpm = 120.0;

    const bool  hold    = apvts.getRawParameterValue ("hold")->load()    > 0.5f;
    const int   divIdx  = (int) apvts.getRawParameterValue ("div")->load();
    const int   autoIdx = (int) apvts.getRawParameterValue ("auto")->load();
    const float pitch   = apvts.getRawParameterValue ("pitch")->load();
    const bool  rev     = apvts.getRawParameterValue ("reverse")->load() > 0.5f;
    const float mix     = apvts.getRawParameterValue ("mix")->load();
    const float fadeMs  = apvts.getRawParameterValue ("smooth")->load();

    const double rate      = std::pow (2.0, pitch / 12.0);
    const double wantSlice = jlimit (16.0, (double) ringSize - 4.0,
                                     kDivBeats[jlimit (0, 5, divIdx)] * 60.0 / bpm * sr);
    const double ppqPerSample = bpm / 60.0 / sr;
    const double autoThresh   = autoIdx == 1 ? 3.0 : (autoIdx == 2 ? 2.0 : 99.0);
    const double fadeSamples  = jmax (4.0, fadeMs * 0.001 * sr);
    const float  actCoef      = 1.0f - std::exp (-1.0f / (float) fadeSamples);

    for (int i = 0; i < numSamples; ++i)
    {
        bool want = hold;
        if (! want && autoIdx > 0 && havePpq)
            want = std::fmod (ppq + i * ppqPerSample, 4.0) >= autoThresh;

        if (want && ! frozen)
        {
            sliceLen = wantSlice;
            frozenStart = (int) (((double) writePos - sliceLen) + ringSize * 2.0) % ringSize;
            phase = 0.0;
        }
        frozen = want;

        act += ((want ? 1.0f : 0.0f) - act) * actCoef;
        const bool playing = frozen || act > 0.001f;

        double offset = 0.0, g = 1.0;
        if (playing)
        {
            phase += 1.0;
            if (phase >= sliceLen) phase -= sliceLen;
            offset = std::fmod (phase * rate, sliceLen);
            if (rev) offset = sliceLen - 1.0 - offset;
            g = jmin (1.0, jmin (phase, sliceLen - phase) / fadeSamples);

            if (offset < 0.0) offset = 0.0;
        }

        for (int ch = 0; ch < numCh; ++ch)
        {
            auto* data = buffer.getWritePointer (ch);
            auto* r    = ring.getWritePointer (ch);
            const float in = data[i];

            if (playing)
            {
                const double pos = frozenStart + offset;
                const int i0 = (int) pos % ringSize;
                const int i1 = (i0 + 1) % ringSize;
                const float fr = (float) (pos - std::floor (pos));
                const float wet = (r[i0] + fr * (r[i1] - r[i0])) * (float) g;
                const float a = act * mix;
                data[i] = in * (1.0f - a) + wet * a;
            }

            if (! frozen && act <= 0.001f)
                r[writePos] = in;
        }

        if (! frozen && act <= 0.001f)
            writePos = (writePos + 1) % ringSize;
    }
}

AudioProcessorEditor* FreeStutterProcessor::createEditor()
{
    return new GenericAudioProcessorEditor (*this);
}

void FreeStutterProcessor::getStateInformation (MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, dest);
}

void FreeStutterProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (ValueTree::fromXml (*xml));
}

AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new FreeStutterProcessor();
}
