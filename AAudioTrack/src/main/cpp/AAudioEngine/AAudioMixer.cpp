//
// Created by matthew good on 18/11/20.
//

#include "AAudioMixer.h"

//void OboeMixer::renderAudio(int16_t *audioData, int32_t numSamples) {
//    // Zero out the incoming container array
//    for (int j = 0; j < numSamples * kChannelCount; ++j) {
//        audioData[j] = 0;
//    }
//
//    for (int i = 0; i < mNextFreeTrackIndex; ++i) {
//        mTracks[i]->renderAudio(mixingBuffer, numSamples, mTracks[i]);
//
//        for (int j = 0; j < numSamples * kChannelCount; ++j) {
//            audioData[j] += mixingBuffer[j];
//        }
//    }
//}

//void OboeMixer::addTrack(SoundRecording *renderer){
//    mTracks[mNextFreeTrackIndex++] = renderer;
//}