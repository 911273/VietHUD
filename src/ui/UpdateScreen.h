#pragma once
#include <Arduino.h>

void buildUpdateScreen();
void openUpdateScreen();
void closeUpdateScreen();
bool isUpdateScreenOpen();
void updateScreenPoll();
// Auto firmware check on Wi-Fi connect + "update now?" prompt (call from loop()).
void updatePromptPoll();
// For button boards: is the "update now?" prompt up, and answer it.
bool updatePromptActive();
void updatePromptAnswer(bool update);
