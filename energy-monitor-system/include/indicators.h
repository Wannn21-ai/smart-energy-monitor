#pragma once

// ================================================================
// indicators.h - Non-blocking LED and buzzer indicator manager
// ================================================================

void indicatorsBegin();
void indicatorsUpdate();

void indicatorsSetWifiSearching(bool searching);
void indicatorsSetCaptivePortalActive(bool active);
void indicatorsResetAlertPattern();

