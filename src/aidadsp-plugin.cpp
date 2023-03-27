/*
 * aidadsp-loader
 * Copyright (C) 2022-2023 Massimo Pennazio <maxipenna@libero.it>
 * Copyright (C) 2023 Filipe Coelho <falktx@falktx.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DistrhoPlugin.hpp"

#include "Biquad.cpp"
#include "ExpSmoother.hpp"
#include "model_variant.hpp"
#include "extra/Sleep.hpp"

#include <atomic>

START_NAMESPACE_DISTRHO

// --------------------------------------------------------------------------------------------------------------------

/* Define a constexpr for converting a gain in dB to a coefficient */
static constexpr const float DB_CO(const float g) { return g > -90.f ? std::pow(10.f, g * 0.05f) : 0.f; }

/* Define a constexpr to scale % to coeff */
static constexpr const float PC_CO(const float g) { return g < 100.f ? (g / 100.f) : 1.f; }

/* Defines for tone controls */
static constexpr const float COMMON_Q = 0.707f;
static constexpr const float DEPTH_FREQ = 75.f;
static constexpr const float PRESENCE_FREQ = 900.f;

// --------------------------------------------------------------------------------------------------------------------

struct AidaToneControl {
    Biquad dc_blocker { bq_type_highpass, 0.5f, COMMON_Q, 0.0f };
    Biquad in_lpf { bq_type_lowpass, 0.5f, COMMON_Q, 0.0f };
    Biquad bass { bq_type_lowshelf, 0.5f, COMMON_Q, 0.0f };
    Biquad mid { bq_type_peak, 0.5f, COMMON_Q, 0.0f };
    Biquad treble { bq_type_highshelf, 0.5f, COMMON_Q, 0.0f };
    Biquad depth { bq_type_highshelf, 0.5f, COMMON_Q, 0.0f };
    Biquad presence { bq_type_highshelf, 0.5f, COMMON_Q, 0.0f };
    ExpSmoother pregain;
    ExpSmoother mastergain;
    bool net_bypass = false;
    bool eq_bypass = false;
    EqPos eq_pos = kEqPost;
    MidEqType mid_type = kMidEqPeak;

    AidaToneControl()
    {
        pregain.setTimeConstant(1);
        mastergain.setTimeConstant(1);
    }

    void setSampleRate(const float parameters[kNumParameters], const double sampleRate)
    {
        dc_blocker.setFc(35.0f / sampleRate);

        in_lpf.setFc(PC_CO(parameters[kParameterINLPF]) * 0.5f);

        bass.setBiquad(bq_type_lowshelf,
                       parameters[kParameterBASSFREQ] / sampleRate, COMMON_Q, parameters[kParameterBASSGAIN]);

        mid.setBiquad(mid_type == kMidEqBandpass ? bq_type_bandpass : bq_type_peak,
                      parameters[kParameterMIDFREQ] / sampleRate,
                      parameters[kParameterMIDQ],
                      parameters[kParameterMIDGAIN]);

        treble.setBiquad(bq_type_highshelf,
                         parameters[kParameterTREBLEFREQ] / sampleRate, COMMON_Q, parameters[kParameterTREBLEGAIN]);

        depth.setBiquad(bq_type_highshelf,
                        DEPTH_FREQ / sampleRate, COMMON_Q, parameters[kParameterDEPTH]);

        presence.setBiquad(bq_type_highshelf,
                           PRESENCE_FREQ / sampleRate, COMMON_Q, parameters[kParameterPRESENCE]);

        pregain.setSampleRate(sampleRate);
        pregain.setTarget(DB_CO(parameters[kParameterPREGAIN]));

        mastergain.setSampleRate(sampleRate);
        mastergain.setTarget(DB_CO(parameters[kParameterMASTER]));
    }
};

struct DynamicModel {
    ModelVariantType variant;
    bool input_skip; /* Means the model has been trained with first input element skipped to the output */
    float output_gain;
};

// --------------------------------------------------------------------------------------------------------------------
// Apply a gain ramp to a buffer

static void applyGainRamp(ExpSmoother& smoother, float* const out, const uint32_t numSamples)
{
    for (uint32_t i=0; i<numSamples; ++i)
        out[i] *= smoother.next();
}

// --------------------------------------------------------------------------------------------------------------------
// Apply filter

static void applyBiquadFilter(Biquad& filter, float* const out, const float* const in, const uint32_t numSamples)
{
    for (uint32_t i=0; i<numSamples; ++i)
        out[i] = filter.process(in[i]);
}

static void applyBiquadFilter(Biquad& filter, float* const out, const uint32_t numSamples)
{
    for (uint32_t i=0; i<numSamples; ++i)
        out[i] = filter.process(out[i]);
}

// --------------------------------------------------------------------------------------------------------------------
// Apply biquad cascade filters

static void applyToneControls(AidaToneControl& aida, float* const out, uint32_t numSamples)
{
    if (aida.mid_type == kMidEqBandpass)
    {
        applyBiquadFilter(aida.mid, out, numSamples);
    }
    else
    {
        applyBiquadFilter(aida.depth, out, numSamples);
        applyBiquadFilter(aida.bass, out, numSamples);
        applyBiquadFilter(aida.mid, out, numSamples);
        applyBiquadFilter(aida.treble, out, numSamples);
        applyBiquadFilter(aida.presence, out, numSamples);
    }
}

// --------------------------------------------------------------------------------------------------------------------
// This function carries model calculations for snapshot models

void applyModel(DynamicModel* model, float* const out, uint32_t numSamples)
{
    const bool input_skip = model->input_skip;
    const float output_gain = model->output_gain;

    std::visit(
        [&input_skip, &out, numSamples, output_gain] (auto&& custom_model)
        {
            using ModelType = std::decay_t<decltype (custom_model)>;
            if constexpr (ModelType::input_size == 1)
            {
                float f;
                if (input_skip)
                {
                    for (uint32_t i=0; i<numSamples; ++i)
                        out[i] += custom_model.forward(out + i) * output_gain;
                }
                else
                {
                    for (uint32_t i=0; i<numSamples; ++i)
                        out[i] = custom_model.forward(out + i) * output_gain;
                }
            }
            else
            {
                // TODO
            }
        },
        model->variant
    );
}

// --------------------------------------------------------------------------------------------------------------------

class AidaDSPLoaderPlugin : public Plugin
{
    AidaToneControl aida;
    DynamicModel* model = nullptr;
    std::atomic<bool> running { false };
    float parameters[kNumParameters];

public:
    AidaDSPLoaderPlugin()
        : Plugin(kNumParameters, 0, 1) // parameters, programs, states
    {
        // Initialize parameters to their defaults
        for (uint i=0; i<kNumParameters; ++i)
            parameters[i] = kParameters[i].ranges.def;

        // initialize
        sampleRateChanged(getSampleRate());
    }

protected:
   /* -----------------------------------------------------------------------------------------------------------------
    * Information */

   /**
      Get the plugin label.
      A plugin label follows the same rules as Parameter::symbol, with the exception that it can start with numbers.
    */
    const char* getLabel() const override
    {
        return "Aida DSP Loader";
    }

   /**
      Get an extensive comment/description about the plugin.
    */
    const char* getDescription() const override
    {
        return "Simple loader for neural models using RTNeural inference engine.";
    }

   /**
      Get the plugin author/maker.
    */
    const char* getMaker() const override
    {
        return DISTRHO_PLUGIN_BRAND;
    }

   /**
      Get the plugin homepage.
    */
    const char* getHomePage() const override
    {
        return "https://aidadsp.github.io/";
    }

   /**
      Get the plugin license name (a single line of text).
      For commercial plugins this should return some short copyright information.
    */
    const char* getLicense() const override
    {
        return "GPL-3.0-or-later";
    }

   /**
      Get the plugin version, in hexadecimal.
    */
    uint32_t getVersion() const override
    {
        return d_version(1, 0, 0);
    }

   /**
      Get the plugin unique Id.
      This value is used by LADSPA, DSSI and VST plugin formats.
    */
    int64_t getUniqueId() const override
    {
        return d_cconst('a', 'i', 'd', 'a');
    }

   /* -----------------------------------------------------------------------------------------------------------------
    * Init */

   /**
      Initialize the audio port @a index.@n
      This function will be called once, shortly after the plugin is created.
    */
    void initAudioPort(bool input, uint32_t index, AudioPort& port) override
    {
        // treat meter audio ports as mono
        port.groupId = kPortGroupMono;

        // everything else is as default
        Plugin::initAudioPort(input, index, port);
    }

   /**
      Initialize the parameter @a index.
      This function will be called once, shortly after the plugin is created.
    */
    void initParameter(uint32_t index, Parameter& parameter) override
    {
        parameter = kParameters[index];
    }

   /**
      Initialize the state @a index.@n
      This function will be called once, shortly after the plugin is created.@n
    */
    void initState(uint32_t index, State& state) override
    {
        if (index != 0)
            return;

        state.hints = kStateIsFilenamePath;
        state.key = "json";
        state.defaultValue = "";
        state.label = "Neural Model";
        state.description = "";
       #ifdef __MOD_DEVICES__
        state.fileTypes = "aidadspmodel";
       #endif
    }

   /* -----------------------------------------------------------------------------------------------------------------
    * Internal data */

   /**
      Get the current value of a parameter.
    */
    float getParameterValue(const uint32_t index) const override
    {
        return parameters[index];
    }

   /**
      Change a parameter value.
    */
    void setParameterValue(const uint32_t index, const float value) override
    {
        parameters[index] = value;

        const double sampleRate = getSampleRate();

        switch (static_cast<Parameters>(index))
        {
        case kParameterINLPF:
            aida.in_lpf.setFc(PC_CO(value) * 0.5f);
            break;
        case kParameterPREGAIN:
            aida.pregain.setTarget(DB_CO(value));
            break;
        case kParameterNETBYPASS:
            aida.net_bypass = value > 0.5f;
            break;
        case kParameterEQBYPASS:
            aida.eq_bypass = value > 0.5f;
            break;
        case kParameterEQPOS:
            aida.eq_pos = value > 0.5f ? kEqPre : kEqPost;
            break;
        case kParameterBASSGAIN:
            aida.bass.setPeakGain(value);
            break;
        case kParameterBASSFREQ:
            aida.bass.setFc(value / sampleRate);
            break;
        case kParameterMIDGAIN:
            aida.mid.setPeakGain(value);
            break;
        case kParameterMIDFREQ:
            aida.mid.setFc(value / sampleRate);
            break;
        case kParameterMIDQ:
            aida.mid.setQ(value);
            break;
        case kParameterMTYPE:
            aida.mid_type = value > 0.5f ? kMidEqBandpass : kMidEqPeak;
            break;
        case kParameterTREBLEGAIN:
            aida.treble.setPeakGain(value);
            break;
        case kParameterTREBLEFREQ:
            aida.treble.setFc(value / sampleRate);
            break;
        case kParameterDEPTH:
            aida.depth.setPeakGain(value);
            break;
        case kParameterPRESENCE:
            aida.presence.setPeakGain(value);
            break;
        case kParameterMASTER:
            aida.mastergain.setTarget(DB_CO(value));
            break;
        case kParameterCount:
            break;
        }
    }

    void setState(const char* const key, const char* const value)
    {
        if (std::strcmp(key, "json") != 0)
            return;

        int input_skip;
        float output_gain;
        nlohmann::json model_json;

        try {
            std::ifstream jsonStream(value, std::ifstream::binary);
            jsonStream >> model_json;

            /* Understand which model type to load */
            if(model_json["in_shape"].back().get<int>() > 1) {
                throw std::invalid_argument("Values for input_size > 1 are not supported");
            }

            if (model_json["in_skip"].is_number()) {
                input_skip = model_json["in_skip"].get<int>();
                if (input_skip > 1)
                    throw std::invalid_argument("Values for in_skip > 1 are not supported");
            }
            else {
                input_skip = 0;
            }

            if (model_json["out_gain"].is_number()) {
                output_gain = DB_CO(model_json["out_gain"].get<float>());
            }
            else {
                output_gain = 1.0f;
            }

            d_stdout("Successfully loaded json file: %s", value);
        }
        catch (const std::exception& e) {
            d_stderr2("Unable to load json file: %s\nError: %s", value, e.what());
            return;
        }

        std::unique_ptr<DynamicModel> newmodel = std::make_unique<DynamicModel>();

        try {
            if (! custom_model_creator (model_json, newmodel->variant))
                throw std::runtime_error ("Unable to identify a known model architecture!");

            std::visit (
                [&model_json] (auto&& custom_model)
                {
                    using ModelType = std::decay_t<decltype (custom_model)>;
                    if constexpr (! std::is_same_v<ModelType, NullModel>)
                    {
                        custom_model.parseJson (model_json, true);
                        custom_model.reset();
                    }
                },
                newmodel->variant);
        }
        catch (const std::exception& e) {
            d_stderr2("Error loading model: %s", e.what());
            return;
        }

        // save extra info
        newmodel->input_skip = input_skip != 0;
        newmodel->output_gain = output_gain;

        // Pre-buffer to avoid "clicks" during initialization
        float out[2048] = {};
        applyModel(newmodel.get(), out, 2048);

        DynamicModel* const oldmodel = model;
        model = newmodel.release();

        // if processing, wait for process cycle to complete
        while (oldmodel != nullptr && running.load())
            d_msleep(5);
    }

   /* -----------------------------------------------------------------------------------------------------------------
    * Process */

   /**
      Activate this plugin.
    */
    void activate() override
    {
        aida.pregain.clearToTarget();
        aida.mastergain.clearToTarget();

        if (model != nullptr)
        {
            running.store(true);

            std::visit (
                [] (auto&& custom_model)
                {
                    using ModelType = std::decay_t<decltype (custom_model)>;
                    if constexpr (! std::is_same_v<ModelType, NullModel>)
                    {
                        custom_model.reset();
                    }
                },
                model->variant);

            running.store(false);
        }
    }

   /**
      Run/process function for plugins without MIDI input.
    */
    void run(const float** inputs, float** outputs, uint32_t numSamples) override
    {
        const float* const in  = inputs[0];
        /* */ float* const out = outputs[0];

        // High frequencies roll-off (lowpass)
        applyBiquadFilter(aida.in_lpf, out, in, numSamples);

        // Pre-gain
        applyGainRamp(aida.pregain, out, numSamples);

        // Equalizer section
        if (!aida.eq_bypass && aida.eq_pos == kEqPre)
            applyToneControls(aida, out, numSamples);

        if (!aida.net_bypass && model != nullptr)
        {
            running.store(true);
            applyModel(model, out, numSamples);
            running.store(false);
        }

        // DC blocker filter (highpass)
        applyBiquadFilter(aida.dc_blocker, out, numSamples);

        // Equalizer section
        if (!aida.eq_bypass && aida.eq_pos == kEqPost)
            applyToneControls(aida, out, numSamples);

        // Master volume
        applyGainRamp(aida.mastergain, out, numSamples);
    }

   /**
      Optional callback to inform the plugin about a sample rate change.@n
      This function will only be called when the plugin is deactivated.
    */
    void sampleRateChanged(const double newSampleRate) override
    {
        aida.setSampleRate(parameters, newSampleRate);
    }

    // ----------------------------------------------------------------------------------------------------------------

   /**
      Set our plugin class as non-copyable and add a leak detector just in case.
    */
    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AidaDSPLoaderPlugin)
};

/* --------------------------------------------------------------------------------------------------------------------
 * Plugin entry point, called by DPF to create a new plugin instance. */

Plugin* createPlugin()
{
    return new AidaDSPLoaderPlugin();
}

// --------------------------------------------------------------------------------------------------------------------

END_NAMESPACE_DISTRHO
