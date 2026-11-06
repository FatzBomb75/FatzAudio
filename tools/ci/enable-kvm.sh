#!/usr/bin/env bash
# Lets every user on a GitHub-hosted Linux runner use KVM (the Android emulator needs it).
set -eu
echo 'KERNEL=="kvm", GROUP="kvm", MODE="0666", OPTIONS+="static_node=kvm"' | sudo tee /etc/udev/rules.d/99-kvm4all.rules
sudo udevadm control --reload-rules
sudo udevadm trigger --name-match=kvm
