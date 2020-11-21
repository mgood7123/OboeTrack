//
// Created by matthew good on 20/11/20.
//

#include "AAudio.h"
#include <memory>

namespace ARDOUR {

    static std::string s_instance_name;
    const size_t _max_buffer_size = 8192;
    static std::shared_ptr<AAudio> _instance;

    #define N_CHANNELS (2)

    AudioBackendInfo AAudio::_descriptor = {
            "AAudio",
            instantiate,
            deinstantiate,
            backend_factory,
            already_configured,
            available
    };

    std::shared_ptr<AudioBackend>
    AAudio::backend_factory (AudioEngine& e)
    {
        if (!_instance) {
            _instance.reset(new AAudio(e, _descriptor));
        }
        return _instance;
    }

    int
    AAudio::instantiate (const std::string& arg1, const std::string& /* arg2 */)
    {
        s_instance_name = arg1;
        return 0;
    }

    int
    AAudio::deinstantiate ()
    {
        _instance.reset ();
        return 0;
    }

    bool
    AAudio::already_configured ()
    {
        return false;
    }

    bool
    AAudio::available ()
    {
        return true;
    }

    ARDOUR::AudioBackendInfo* AAudio::descriptor ()
    {
        return &_descriptor;
    }

    // make sure that the streams are stopped AND CLOSED when your app
    // is in the background and not being used.
    // Keeping an open stream may keep the DSP open and drawing power.
    // Also an open stream can hold resources that may be needed by another app.

    AAudio::AAudio(AudioEngine &audioEngine, AudioBackendInfo& i)
        : AudioBackend(audioEngine, i)
        , currentDeviceStatus("default", false)
        , currentSampleRate(0.0)
        , currentBufferSizeInFrames(0)
        , currentInputChannelCount(0)
        , currentOutputChannelCount(0)
        {
        _instance_name = s_instance_name;
        CreateStream();
    }

    int AAudio::_start(bool for_latency_measurement) {
        LOGE("_start called()");
        StartStreamBlocking();
        return 0;
    }

    int AAudio::stop() {
        drop_device();
        return 0;
    }

    int AAudio::drop_device() {
        StopStreamBlocking();
        FlushStreamBlocking();
        DestroyStream();
        return 0;
    }

    AAudio::~AAudio() {}

    aaudio_data_callback_result_t AAudio::onAudioReady(
            AAudioStream *stream, void *userData, void *audioData,
            frames_t number_of_frames_to_render
    ) {
        AAudio *aaudio = static_cast<AAudio *>(userData);

        aaudio->engine.renderAudio(audioData, number_of_frames_to_render);

        aaudio->_processed_samples += number_of_frames_to_render;

        // Are we getting underruns?
        frames_t tmpuc = AAudioStream_getXRunCount(stream);
        if (tmpuc > aaudio->previousUnderrunCount) {
            aaudio->previousUnderrunCount = aaudio->underrunCount;
            aaudio->underrunCount = tmpuc;
            // Try increasing the buffer size by one burst
            frames_t bufferSize = aaudio->bufferSize;
            aaudio->bufferSize += aaudio->framesPerBurst;
            aaudio->bufferSize = AAudioStream_setBufferSizeInFrames(stream, aaudio->bufferSize);
            LOGW("onAudioReady: aaudio->bufferSize increased from %d to %d", bufferSize,
                 aaudio->bufferSize);
        }
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    void AAudio::onError(AAudioStream *stream, void *userData, aaudio_result_t error) {
        if (error == AAUDIO_ERROR_DISCONNECTED) {
            std::function<void(void)> restartFunction = std::bind(&AAudio::RestartStreamNonBlocking,
                                                                  static_cast<AAudio *>(userData));
            new std::thread(restartFunction);
        }
    }

    aaudio_result_t AAudio::waitForState(aaudio_stream_state_t streamState) {
        aaudio_result_t result = AAUDIO_OK;
        aaudio_stream_state_t currentState = AAudioStream_getState(stream);
        aaudio_stream_state_t inputState = currentState;
        while ((result == AAUDIO_OK || result == AAUDIO_ERROR_TIMEOUT) && currentState != streamState) {
            result = AAudioStream_waitForStateChange(stream, inputState, &currentState, 2000);
            inputState = currentState;
        }
        return result;
    }

    aaudio_result_t AAudio::CreateStream() {
        aaudio_result_t result = AAudio_createStreamBuilder(&builder);
        if (result != AAUDIO_OK) {
            LOGE("FAILED TO CREATE STREAM BUILDER: %s", AAudio_convertResultToText(result));
            return result;
        }

        AAudioStreamBuilder_setDataCallback(builder, onAudioReady, this);
        AAudioStreamBuilder_setErrorCallback(builder, onError, this);
        AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
        result = AAudioStreamBuilder_openStream(builder, &stream);
        if (result != AAUDIO_OK) {
            LOGE("FAILED TO OPEN THE STREAM: %s", AAudio_convertResultToText(result));
            return result;
        }
        underrunCount = 0;
        previousUnderrunCount = 0;
        return AAUDIO_OK;
    }

    aaudio_result_t AAudio::DestroyStream() {
        aaudio_result_t result = AAudioStream_close(stream);
        if (result != AAUDIO_OK) {
            LOGE("FAILED TO CLOSE STREAM BUILDER: %s", AAudio_convertResultToText(result));
            return result;
        }
        result = AAudioStreamBuilder_delete(builder);
        if (result != AAUDIO_OK) {
            LOGE("FAILED TO DELETE STREAM BUILDER: %s", AAudio_convertResultToText(result));
            return result;
        }
        stream = nullptr;
        builder = nullptr;
        return result;
    }

    void AAudio::RestartStreamNonBlocking() {
        StopStreamNonBlocking();
        FlushStreamNonBlocking();
        CreateStream();
        StartStreamNonBlocking();
    }

    void AAudio::RestartStreamBlocking() {
        StopStreamBlocking();
        FlushStreamBlocking();
        CreateStream();
        StartStreamBlocking();
    }

    aaudio_result_t AAudio::StartStreamNonBlocking() {
        aaudio_result_t result = AAudioStream_requestStart(stream);
        if (result != AAUDIO_OK) LOGE("FAILED TO START THE STREAM: %s", AAudio_convertResultToText(result));
        return result;
    }

    aaudio_result_t AAudio::StartStreamBlocking() {
        aaudio_result_t result = AAudioStream_requestStart(stream);
        if (result != AAUDIO_OK) LOGE("FAILED TO START THE STREAM: %s", AAudio_convertResultToText(result));
        else {
            result = waitForState(AAUDIO_STREAM_STATE_STARTED);
            if (result != AAUDIO_OK) LOGE("FAILED TO WAIT FOR STATE CHANGE: %s", AAudio_convertResultToText(result));
        }
        return result;
    }

    aaudio_result_t AAudio::PauseStreamNonBlocking() {
        aaudio_result_t result = AAudioStream_requestPause(stream);
        if (result != AAUDIO_OK) LOGE("FAILED TO PAUSE THE STREAM: %s", AAudio_convertResultToText(result));
        return result;
    }

    aaudio_result_t AAudio::PauseStreamBlocking() {
        aaudio_result_t result = AAudioStream_requestPause(stream);
        if (result != AAUDIO_OK) LOGE("FAILED TO PAUSE THE STREAM: %s", AAudio_convertResultToText(result));
        else {
            result = waitForState(AAUDIO_STREAM_STATE_PAUSED);
            if (result != AAUDIO_OK) LOGE("FAILED TO WAIT FOR STATE CHANGE");
        }
        return result;
    }

    aaudio_result_t AAudio::StopStreamNonBlocking() {
        aaudio_result_t result = AAudioStream_requestStop(stream);
        if (result != AAUDIO_OK) LOGE("FAILED TO STOP THE STREAM: %s", AAudio_convertResultToText(result));
        return result;
    }

    aaudio_result_t AAudio::StopStreamBlocking() {
        aaudio_result_t result = AAudioStream_requestStop(stream);
        if (result != AAUDIO_OK) LOGE("FAILED TO STOP THE STREAM: %s", AAudio_convertResultToText(result));
        else {
            result = waitForState(AAUDIO_STREAM_STATE_STOPPED);
            if (result != AAUDIO_OK) LOGE("FAILED TO WAIT FOR STATE CHANGE");
        }
        return result;
    }

    aaudio_result_t AAudio::FlushStreamNonBlocking() {
        aaudio_result_t result = AAudioStream_requestFlush(stream);
        if (result != AAUDIO_OK) LOGE("FAILED TO FLUSH THE STREAM: %s", AAudio_convertResultToText(result));
        return result;
    }

    aaudio_result_t AAudio::FlushStreamBlocking() {
        aaudio_result_t result = AAudioStream_requestFlush(stream);
        if (result != AAUDIO_OK) LOGE("FAILED TO FLUSH THE STREAM: %s", AAudio_convertResultToText(result));
        else {
            result = waitForState(AAUDIO_STREAM_STATE_FLUSHED);
            if (result != AAUDIO_OK) LOGE("FAILED TO WAIT FOR STATE CHANGE");
        }
        return result;
    }


    std::string AAudio::name() const {
        return std::string();
    }

    bool AAudio::is_realtime() const {
        return true;
    }


    std::vector<AAudio::DeviceStatus> AAudio::enumerate_devices() const {
        // ignore device
        const_cast<DeviceStatus *>(&currentDeviceStatus)[0] = DeviceStatus(std::to_string(AAudioStream_getDeviceId(stream)), true);
        return std::vector<DeviceStatus>(1, currentDeviceStatus);
    }

    std::vector<float> AAudio::available_sample_rates(const std::string &device) const {
        // ignore device
        const_cast<float*>(&currentSampleRate)[0] = AAudioStream_getSampleRate(stream);
        return std::vector<float>(1,currentSampleRate);
    }

    std::vector<uint32_t> AAudio::available_buffer_sizes(const std::string &device) const {
        // ignore device
        const_cast<int32_t *>(&currentBufferSizeInFrames)[0] = AAudioStream_getBufferSizeInFrames(stream);
        return std::vector<uint32_t>(1, currentBufferSizeInFrames);
    }

    uint32_t AAudio::available_input_channel_count(const std::string &device) const {
        // ignore device
        const_cast<int32_t *>(&currentInputChannelCount)[0] = AAudioStream_getChannelCount(stream);
        return currentInputChannelCount;
    }

    uint32_t AAudio::available_output_channel_count(const std::string &device) const {
        // ignore device
        const_cast<int32_t *>(&currentOutputChannelCount)[0] = AAudioStream_getChannelCount(stream);
        return currentOutputChannelCount;
    }

    bool AAudio::can_change_sample_rate_when_running() const {
        return false;
    }

    bool AAudio::can_change_buffer_size_when_running() const {
        return true;
    }

    bool AAudio::can_measure_systemic_latency() const {
        return false;
    }

    int AAudio::set_device_name(const std::string &string) {
        return 0;
    }

    int AAudio::set_sample_rate(float d) {
        return 0;
    }

    int AAudio::set_buffer_size(uint32_t uint32) {
        return 0;
    }

    int AAudio::set_interleaved(bool yn) {
        return 0;
    }

    int AAudio::set_input_channels(uint32_t uint32) {
        return 0;
    }

    int AAudio::set_output_channels(uint32_t uint32) {
        return 0;
    }

    int AAudio::set_systemic_input_latency(uint32_t uint32) {
        return 0;
    }

    int AAudio::set_systemic_output_latency(uint32_t uint32) {
        return 0;
    }

    int AAudio::set_systemic_midi_input_latency(std::string string, uint32_t uint32) {
        return 0;
    }

    int AAudio::set_systemic_midi_output_latency(std::string string, uint32_t uint32) {
        return 0;
    }

    std::string AAudio::device_name() const {
        return std::string();
    }

    float AAudio::sample_rate() const {
        return AAudioStream_getSampleRate(stream);
    }

    uint32_t AAudio::buffer_size() const {
        return AAudioStream_getBufferSizeInFrames(stream);
    }

    bool AAudio::interleaved() const {
        return false;
    }

    uint32_t AAudio::input_channels() const {
        return AAudioStream_getChannelCount(stream);
    }

    uint32_t AAudio::output_channels() const {
        return AAudioStream_getChannelCount(stream);
    }

    uint32_t AAudio::systemic_input_latency() const {
        return 0;
    }

    uint32_t AAudio::systemic_output_latency() const {
        return 0;
    }

    uint32_t AAudio::systemic_midi_input_latency(std::string string) const {
        return 0;
    }

    uint32_t AAudio::systemic_midi_output_latency(std::string string) const {
        return 0;
    }

    std::string AAudio::control_app_name() const {
        return std::string();
    }

    void AAudio::launch_control_app() {

    }

    std::vector<std::string> AAudio::enumerate_midi_options() const {
        return std::vector<std::string>();
    }

    int AAudio::set_midi_option(const std::string &option) {
        return 0;
    }

    std::string AAudio::midi_option() const {
        return std::string();
    }

    std::vector<AAudio::DeviceStatus> AAudio::enumerate_midi_devices() const {
        return std::vector<DeviceStatus>();
    }

    int AAudio::set_midi_device_enabled(std::string string, bool b) {
        return 0;
    }

    bool AAudio::midi_device_enabled(std::string string) const {
        return false;
    }

    bool AAudio::can_set_systemic_midi_latencies() const {
        return false;
    }

    int AAudio::reset_device() {
        return 0;
    }

    int AAudio::freewheel(bool start_stop) {
        return 0;
    }

    float AAudio::dsp_load() const {
        return 0;
    }

    sample_position_t AAudio::sample_time() {
        return 0;
    }

    sample_position_t AAudio::sample_time_at_cycle_start() {
        return 0;
    }

    frames_t AAudio::samples_since_cycle_start() {
        return 0;
    }

    int AAudio::create_process_thread(std::function<void()> func) {
        return 0;
    }

    int AAudio::join_process_threads() {
        return 0;
    }

    bool AAudio::in_process_thread() {
        return false;
    }

    uint32_t AAudio::process_thread_count() {
        return 0;
    }

    void AAudio::update_latencies() {

    }

}