/*
 * audio.h - active buzzer (low-level trigger) on GPIO47.
 *
 * The module is an ML-type active buzzer with VCC / I/O / GND pins. It has its
 * own oscillator: applying a LOW level to I/O turns it on, HIGH turns it off.
 * The pitch is fixed, so cues are patterns of on/off beeps separated by pauses,
 * not different frequencies.
 */
#ifndef AUDIO_H
#define AUDIO_H

#include "esp_err.h"

/* Each cue is a fixed sequence of beeps and gaps. */
typedef enum {
    AUDIO_TARE = 0,       /* one short beep           */
    AUDIO_STABLE,         /* one medium beep          */
    AUDIO_AZT_ON,         /* two beeps                */
    AUDIO_AZT_OFF,        /* two beeps, longer gap    */
    AUDIO_CAL_START,      /* three short beeps        */
    AUDIO_PLACE_WEIGHT,   /* one long beep            */
    AUDIO_CAL_DONE,       /* three ascending-style beeps (longer gaps) */
    AUDIO_ERROR,          /* three long beeps         */
    AUDIO_COUNT,
} audio_cue_t;

esp_err_t audio_init(void);

/* Non-blocking: the pattern is played by a task on core 1. A new request while
 * one is playing replaces it, so cues never queue up. */
void audio_play(audio_cue_t cue);

/* Console helper: play cue number `index` once. */
void audio_test(int index);

#endif /* AUDIO_H */
