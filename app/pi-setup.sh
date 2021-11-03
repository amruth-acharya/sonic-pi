#!/bin/bash

echo "Fetching dependencies via apt..."

sudo apt-get install -y build-essential git ruby-dev elixir erlang-dev qttools5-dev qttools5-dev-tools libqt5svg5-dev supercollider-server sc3-plugins-server alsa-utils jackd2 libjack-jackd2-0 pulseaudio-module-jack librtmidi-dev cmake ninja-build
