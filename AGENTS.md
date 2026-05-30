# AGENTS.md

## Project Identity

This is an ESP32 smart energy monitoring project using:
- ESP32
- PZEM-004T + CT
- Relay
- OLED
- LittleFS
- Firebase Realtime Database
- Online web dashboard built with HTML/CSS/JS
- 3 LEDs
- 1 buzzer

Do not add new hardware, sensors, modules, or external devices.

Allowed hardware only:
- MCB
- Terminal block
- PZEM-004T + CT
- HLK power module
- Relay
- ESP32
- OLED
- 3 LEDs
- 1 buzzer

## Main Architecture Rules

Always preserve this architecture:

1. SessionData is the single source of truth.
2. OLED displays realtime data from SessionData.
3. Web dashboard mirrors/syncs from SessionData/Firebase.
4. Every stopped session must be saved to LittleFS first.
5. Firebase is cloud sync, not the first source of history.
6. Do not hardcode user UID in ESP32.
7. The Firebase user UID must come from the currently logged-in web user.
8. ESP32 must not write directly to /users/{uid}/history.
9. ESP32 saves completed sessions to LittleFS first, then may upload to:
   /devices/{deviceId}/completedSessions/{sessionId}
10. The authenticated online web dashboard copies completed sessions to:
   /users/{currentUser.uid}/history/{sessionId}

## History Flow

All stop conditions must call one finalizeSession(reason) flow.

Stop conditions:
- Stop Session from web
- load/device unplugged
- overload
- offline session finished
- blackout recovery finalized

Required stop flow:
1. Take last valid SessionData snapshot.
2. Save final history entry to LittleFS.
3. Mark syncStatus = PENDING.
4. Stop monitoring state.
5. Turn relay OFF when required.
6. Upload to Firebase device queue only if available.
7. Never delete local LittleFS history when Firebase fails.
8. Prevent duplicate history entries by using sessionId as unique key.

## Firebase Rules

The online web dashboard is authenticated with Firebase Auth.
The ESP32 must not hardcode user UID.
The online web dashboard must use currentUser.uid.

Use these Firebase paths:
- /devices/{deviceId}/live
- /devices/{deviceId}/completedSessions/{sessionId}
- /users/{uid}/history/{sessionId}
- /users/{uid}/settings

## Required History Fields

Each history entry should include:
- id
- deviceId
- name
- voltage
- current
- power
- energy
- frequency
- powerFactor
- cost
- tariff
- duration
- startTime
- endTime
- date
- timestamp
- startMode
- endMode
- endReason
- overload
- overloadThreshold
- syncStatus
- createdFrom

Minimum Firebase validation fields:
- name
- duration
- power
- energy
- cost
- date
- timestamp

## Online / Offline Logic

Online mode:
- Relay starts OFF.
- User must press Start Monitoring from web.
- Relay turns ON after start command.
- After a short delay, PZEM validates load.
- If no valid load, cancel session and relay OFF.

Offline mode:
- Triggered by WiFi timeout, button command, or captive portal offline option.
- Relay ON automatically only for first offline device.
- Next offline device requires button press.
- OLED is primary realtime display.

Transition:
- Online to offline: OLED continues, web freezes.
- Offline to online: web resyncs from latest session values.
- Web duration must not keep counting when disconnected.

## Overload Logic

Overload threshold comes from saved config.
Captive portal and web settings must stay synchronized.

If power approaches threshold:
- red LED + buzzer warning
- relay stays ON
- warning stops if power drops

If power >= threshold:
- save last valid snapshot
- relay OFF immediately
- alarm LED + buzzer for 10 seconds
- finalizeSession(OVERLOAD)
- save history with overload status

## Coding Rules

- Do not rewrite the whole project.
- Apply minimal safe changes.
- Analyze before editing.
- Keep existing features working.
- Do not modify unrelated UI or HTML unless required.
- Avoid blocking delay().
- Use millis()-based timing for LED, buzzer, timeout, reconnect, and debounce.
- Keep section comments clear and consistent.
- Preserve pin mapping unless explicitly asked.
- Do not add dependencies unless asked.
- Do not delete LittleFS data logic.
- Do not remove Firebase Auth logic from the web.

## Required Code Organization

Use clear section comments:

// ==================================================
// SESSION MANAGER
// ==================================================

// ==================================================
// WIFI MANAGER
// ==================================================

// ==================================================
// RELAY MANAGER
// ==================================================

// ==================================================
// OVERLOAD MANAGER
// ==================================================

// ==================================================
// FIREBASE SYNC
// ==================================================

// ==================================================
// LITTLEFS STORAGE
// ==================================================

// ==================================================
// WEB DASHBOARD
// ==================================================

// ==================================================
// OLED DISPLAY
// ==================================================

// ==================================================
// LED AND BUZZER MANAGER
// ==================================================

## Before Editing

Before making code changes:
1. Explain the current flow.
2. Identify the exact bug.
3. Identify affected files/functions.
4. Propose a minimal patch.
5. Then edit only the necessary code.

## Testing Requirements

After changes, tell me what to test:
- online stop session
- online to offline to stop
- offline stop
- Firebase sync
- duplicate history prevention
- pending sync count
- web duration freeze
- OLED/web sync