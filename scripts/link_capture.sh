#!/bin/bash

pw-link -d pw-play:output_FL $SPEAKER_OUTPUT_L
pw-link -d pw-play:output_FR $SPEAKER_OUTPUT_R

pw-link pw-play:output_FL signal_forge:input