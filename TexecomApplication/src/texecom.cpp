// Copyright 2019 Kevin Cooper

#include "texecom.h"

Texecom::Texecom(void (*callback)(CALLBACK_TYPE, uint8_t, uint8_t, const char*)) {
    this->callback = callback;
    userCount = sizeof(users) / sizeof(char*);
}

void Texecom::setUDLCode(const char *code) {
    if (strlen(code) == 6) {
        strcpy(udlCode, code);
        EEPROM.put(0, udlCode);
    }
    Log.info("New UDL code = %s", udlCode);
}

void Texecom::sendTest(const char *text) {
    if (strlen(text) == 0) {
        if (activeProtocol != SIMPLE) {
            Log.info("Simple login required");
            simpleTask = SIMPLE_LOGIN;
        } else if (activeProtocol == SIMPLE) {
            Log.info("Simple login valid");
        }
    } else if (strcmp(text, "STATUS") == 0) {
        Log.info("Requesting arm state");
        requestArmState();
    } else if (strcmp(text, "LOGOUT") == 0) {
        simpleTask = SIMPLE_LOGOUT;
        texSerial.print("\\H/");
    } else if (strcmp(text, "IDENTITY") == 0) {
        texSerial.print("\\I/");
    } else if (strcmp(text, "FULL-ARM") == 0) {
        Log.info("Full arming");
        texSerial.print("\\A");
        texSerial.write(1);
        texSerial.print("/");
    } else if (strcmp(text, "PART-ARM") == 0) {
        Log.info("Part arming");
        texSerial.print("\\Y");
        texSerial.write(1);
        texSerial.print("/");
    } else if (text[0] == 'T') {
        if (strlen(text) == 2 && text[1] == '?') {
            texSerial.print("\\T?/");
        } else {
            texSerial.print("\\T");
            texSerial.write(Time.day());
            texSerial.write(Time.month());
            texSerial.write(Time.year()-2000);
            texSerial.write(Time.hour());
            texSerial.write(Time.minute());
            texSerial.write("/");
        }
    }
}

void Texecom::request(COMMAND command) {
    texSerial.println(commandStrings[command]);
    lastCommandTime = millis();
}

void Texecom::disarm(const char *code) {
    if (currentTask != CRESTRON_IDLE) {
        Log.info("DISARMING: Request already in progress");
        return;
    }
    if (strlen(code) <= 8)
        snprintf(userPin, sizeof(userPin), code);
    else
        return;
    task = DISARM;
    currentTask = CRESTRON_START;
    disarmSystem(RESULT_NONE);
}

void Texecom::arm(const char *code, ARM_TYPE type) {
    if (currentTask != CRESTRON_IDLE) {
        Log.info("ARMING: Request already in progress");
        return;
    }
    if (strlen(code) <= 8)
        snprintf(userPin, sizeof(userPin), code);
    else
        return;
    armType = type;
    task = ARM;
    currentTask = CRESTRON_START;
    armSystem(RESULT_NONE);
}

void Texecom::requestArmState() {
    // lastStateCheck = millis();
    request(COMMAND_ARMED_STATE);
}

void Texecom::requestScreen() {
    request(COMMAND_SCREEN_STATE);
}

void Texecom::delayCommand(COMMAND command, int delay) {
    delayedCommand = command;
    delayedCommandExecuteTime = millis() + delay;
}

void Texecom::updateAlarmState(ALARM_STATE state) {
    if (alarmState == state)
        return;

    if (state == ARMED && (alarmState == ARMED_HOME || alarmState == ARMED_AWAY))
        return;

    if (state == ARMED)
        delayCommand(COMMAND_SCREEN_STATE, 1000);

    if (state == DISARMED)
        triggeredZone = 0;

    if (state == TRIGGERED && triggeredZone != 0)
            callback(ALARM_TRIGGERED, triggeredZone, 0, NULL);

    lastStateChange = millis();
    alarmState = state;
    callback(ALARM_STATE_CHANGE, 0, alarmState, NULL);
}

void Texecom::updateZoneState(char *message) {
    uint8_t zone;
    uint8_t state;

    char zoneChar[4];
    memcpy(zoneChar, &message[2], 3);
    zoneChar[3] = '\0';
    zone = atoi(zoneChar);

    if ((alarmState == PENDING || alarmState == TRIGGERED) &&
        triggeredZone == 0) {
        triggeredZone = zone;

        if (alarmState == TRIGGERED)
            callback(ALARM_TRIGGERED, 0, triggeredZone, NULL);
    }

    state = message[5] - '0';

    callback(ZONE_STATE_CHANGE, zone, state, NULL);
}

void Texecom::processTask(RESULT result) {
    if (result == TASK_TIMEOUT)
        Log.info("processTask: Task timed out");

    if (task == ARM) {
        armSystem(result);
    } else if (task == DISARM) {
        disarmSystem(result);
    }
}

void Texecom::disarmSystem(RESULT result) {
    switch (currentTask) {
        case CRESTRON_START :
            disarmStartTime = millis();
            Log.info("DISARMING: Starting disarm process");
            currentTask = CRESTRON_CONFIRM_ARMED;
            requestArmState();
            break;

        case CRESTRON_CONFIRM_ARMED :
            if (result == IS_ARMED) {
                Log.info("DISARMING: Confirmed armed. Confirming idle screen");
                currentTask = CRESTRON_CONFIRM_IDLE_SCREEN;
                requestScreen();
            } else if (result == IS_DISARMED) {
                Log.info("DISARMING: System already armed. Aborting");
                abortTask();
            } else {
                abortTask();
            }
            break;

        case CRESTRON_CONFIRM_IDLE_SCREEN :
            if (
                result == SCREEN_IDLE ||
                result == SCREEN_PART_ARMED ||
                result == SCREEN_FULL_ARMED ||
                result == SCREEN_AREA_ENTRY) {
                Log.info("DISARMING: Idle screen confirmed. Starting login process");
                performLogin = true;
                currentTask = CRESTRON_LOGIN;
            } else {
                Log.info("DISARMING: Screen is not idle. Aborting");
                abortTask();
            }
            break;

        case CRESTRON_LOGIN:
            if (result == LOGIN_COMPLETE) {
                Log.info("DISARMING: Login complete. Awaiting confirmed login");
                currentTask = CRESTRON_LOGIN_WAIT;
            } else {
                Log.info("DISARMING: Login failed. Aborting");
                abortTask();
            }
            break;

        case CRESTRON_LOGIN_WAIT :
            if (result == LOGIN_CONFIRMED) {
                if (alarmState != PENDING) {
                    Log.info("DISARMING: Login confirmed. Waiting for Disarm prompt");
                    currentTask = CRESTRON_WAIT_FOR_DISARM_PROMPT;
                    delayCommand(COMMAND_SCREEN_STATE, 500);
                } else {
                    Log.info("DISARMING: Login confirmed. Waiting for Disarm confirmation");
                    currentTask = CRESTRON_DISARM_REQUESTED;
                }
            } else {
                Log.info("DISARMING: Login failed to confirm. Aborting");
                abortTask();
            }
            break;

        case CRESTRON_WAIT_FOR_DISARM_PROMPT :
            if (result == DISARM_PROMPT) {
                Log.info("DISARMING: Disarm prompt confirmed, disarming");
                if (!debugMode)
                    texSerial.println("KEYY");  // Yes

                currentTask = CRESTRON_DISARM_REQUESTED;
            } else {
                Log.info("DISARMING: Unexpected result at WAIT_FOR_DISARM_PROMPT. Aborting");
                abortTask();
            }
            break;

        case CRESTRON_DISARM_REQUESTED :
            if (result == IS_DISARMED) {
                Log.info("DISARMING: DISARM CONFIRMED");
                currentTask = CRESTRON_IDLE;
                memset(userPin, 0, sizeof userPin);
                disarmStartTime = 0;
            } else {
                Log.info("DISARMING: Unexpected result at DISARM_REQUESTED. Aborting");
                abortTask();
            }
            break;
    }

    if (result == TASK_TIMEOUT && currentTask != CRESTRON_IDLE)
        abortTask();
}

void Texecom::armSystem(RESULT result) {
    switch (currentTask) {
        case CRESTRON_START :  // Initiate request
            if (armType == FULL_ARM)
                Log.info("ARMING: Starting full arm process");
            else if (armType == NIGHT_ARM)
                Log.info("ARMING: Starting night arm process");
            else
                return;

            armStartTime = millis();
            Log.info("ARMING: Requesting arm state");
            currentTask = CRESTRON_CONFIRM_DISARMED;
            requestArmState();
            break;

        case CRESTRON_CONFIRM_DISARMED:
            if (result == IS_DISARMED) {
                Log.info("ARMING: Confirmed disarmed. Confirming idle screen");
                currentTask = CRESTRON_CONFIRM_IDLE_SCREEN;
                requestScreen();
            } else if (result == IS_ARMED) {
                Log.info("ARMING: System already armed. Aborting");
                abortTask();
            } else {
                abortTask();
            }
            break;

        case CRESTRON_CONFIRM_IDLE_SCREEN :
            if (result == SCREEN_IDLE) {
                Log.info("ARMING: Idle screen confirmed. Starting login process");
                performLogin = true;
                currentTask = CRESTRON_LOGIN;
            } else {
                Log.info("ARMING: Screen is not idle. Aborting");
                abortTask();
            }
            break;

        case CRESTRON_LOGIN:
            if (result == LOGIN_COMPLETE) {
                Log.info("ARMING: Login complete. Awaiting confirmed login");
                currentTask = CRESTRON_LOGIN_WAIT;
            } else {
                Log.info("ARMING: Login failed. Aborting");
                abortTask();
            }
            break;

        case CRESTRON_LOGIN_WAIT :
            if (result == LOGIN_CONFIRMED) {
                Log.info("ARMING: Login confirmed. Waiting for Arm prompt");
                currentTask = CRESTRON_WAIT_FOR_ARM_PROMPT;
                delayCommand(COMMAND_SCREEN_STATE, 500);
            } else {
                Log.info("ARMING: Login failed to confirm. Aborting");
                abortTask();
            }
            break;

        case CRESTRON_WAIT_FOR_ARM_PROMPT :
            if (result == FULL_ARM_PROMPT) {
                if (armType == FULL_ARM) {
                    Log.info("ARMING: Full arm prompt confirmed, completing full arm");
                    if (!debugMode)
                        texSerial.println("KEYY");  // Yes
                    currentTask = CRESTRON_ARM_REQUESTED;
                } else if (armType == NIGHT_ARM) {
                    Log.info("ARMING: Full arm prompt confirmed, waiting for part arm prompt");
                    currentTask = CRESTRON_WAIT_FOR_PART_ARM_PROMPT;
                    texSerial.println("KEYD");  // Down
                    delayCommand(COMMAND_SCREEN_STATE, 500);
                }
            } else {
                Log.info("ARMING: Unexpected result at WAIT_FOR_ARM_PROMPT. Aborting");
                abortTask();
            }
            break;

        case CRESTRON_WAIT_FOR_PART_ARM_PROMPT :
            if (result == PART_ARM_PROMPT) {
                Log.info("ARMING: Part arm prompt confirmed, waiting for night arm prompt");
                currentTask = CRESTRON_WAIT_FOR_NIGHT_ARM_PROMPT;
                texSerial.println("KEYY");  // Yes
                delayCommand(COMMAND_SCREEN_STATE, 500);
            } else {
                Log.info("ARMING: Unexpected result at WAIT_FOR_PART_ARM_PROMPT. Aborting");
                abortTask();
            }
            break;

        case CRESTRON_WAIT_FOR_NIGHT_ARM_PROMPT :
            if (result == NIGHT_ARM_PROMPT) {
                Log.info("ARMING: Night arm prompt confirmed, Completing part arm");
                if (!debugMode)
                    texSerial.println("KEYY");  // Yes
                currentTask = CRESTRON_ARM_REQUESTED;
            } else {
                Log.info("ARMING: Unexpected result at WAIT_FOR_NIGHT_ARM_PROMPT. Aborting");
                abortTask();
            }
            break;

        case CRESTRON_ARM_REQUESTED :
            if (result == IS_ARMING) {
                Log.info("ARMING: ARM CONFIRMED");
                currentTask = CRESTRON_IDLE;
                memset(userPin, 0, sizeof userPin);
                armStartTime = 0;
                delayCommand(COMMAND_SCREEN_STATE, 2500);
            } else {
                Log.info("ARMING: Unexpected result at ARM_REQUESTED. Aborting");
                abortTask();
            }
            break;
    }

    if (result == TASK_TIMEOUT && currentTask != CRESTRON_IDLE)
        abortTask();
}

void Texecom::abortTask() {
    currentTask = CRESTRON_IDLE;
    texSerial.println("KEYR");
    delayedCommandExecuteTime = 0;
    memset(userPin, 0, sizeof userPin);
    performLogin = false;
    nextPinEntryTime = 0;
    armStartTime = 0;
    disarmStartTime = 0;
    requestArmState();

    lastCommandTime = 0;
    commandAttempts = 0;
}

void Texecom::checkDigiOutputs() {

    bool _state = digitalRead(pinFullArmed);

    if (_state != statePinFullArmed) {
        statePinFullArmed = _state;
        if (_state == LOW)
            updateAlarmState(ARMED_AWAY);
        else
            updateAlarmState(DISARMED);
    }

    _state = digitalRead(pinPartArmed);

    if (_state != statePinPartArmed) {
        statePinPartArmed = _state;
        if (_state == LOW)
            updateAlarmState(ARMED_HOME);
        else
            updateAlarmState(DISARMED);
    }

    _state = digitalRead(pinEntry);

    if (_state != statePinEntry) {
        statePinEntry = _state;
        if (_state == LOW)
            updateAlarmState(PENDING);
    }

    _state = digitalRead(pinExiting);

    if (_state != statePinExiting) {
        statePinExiting = _state;
        if (_state == LOW)
            updateAlarmState(PENDING);
    }

    _state = digitalRead(pinTriggered);

    if (_state != statePinTriggered) {
        statePinTriggered = _state;
        if (_state == LOW)
            updateAlarmState(TRIGGERED);
    }

    _state = digitalRead(pinAreaReady);

    if (_state != statePinAreaReady) {
        statePinAreaReady = _state;
        callback(ALARM_READY_CHANGE, 0, statePinAreaReady == LOW ? 1 : 0, NULL);
    }

    _state = digitalRead(pinFaultPresent);

    if (_state != statePinFaultPresent) {
        statePinFaultPresent = _state;
        if (_state == LOW) {
            callback(SEND_MESSAGE, 0, 0, "Alarm is reporting a fault");
            Log.error("Alarm is reporting a fault");
        } else {
            callback(SEND_MESSAGE, 0, 0, "Alarm fault is resolved");
            Log.info("Alarm fault is resolved");
        }
    }

    _state = digitalRead(pinArmFailed);

    if (_state != statePinArmFailed) {
        statePinArmFailed = _state;
        if (_state == LOW) {
            callback(SEND_MESSAGE, 0, 0, "Alarm failed to arm");
            Log.error("Alarm failed to arm");
        }
    }

}

void Texecom::setup() {
    texSerial.begin(19200, SERIAL_8N1);  // open serial communications
    
    pinMode(pinFullArmed, INPUT);
    pinMode(pinPartArmed, INPUT);
    pinMode(pinEntry, INPUT);
    pinMode(pinExiting, INPUT);
    pinMode(pinTriggered, INPUT);
    pinMode(pinArmFailed, INPUT);
    pinMode(pinFaultPresent, INPUT);
    pinMode(pinAreaReady, INPUT);

    EEPROM.get(0, udlCode);
    Log.info("UDL code = %s", udlCode);
    
    checkDigiOutputs();

    if (alarmState == UNKNOWN) {
        updateAlarmState(DISARMED);
    }
}

void Texecom::loop() {
    bool messageReady = false;
    bool messageComplete = true;

    // Read incoming serial data if available and copy to TCP port
    while (texSerial.available() > 0) {
        int incomingByte = texSerial.read();
        if (bufferPosition == 0)
            messageStart = millis();

        if (bufferPosition >= maxMessageSize) {
            memcpy(message, buffer, bufferPosition);
            message[bufferPosition] = '\0';
            messageReady = true;
            bufferPosition = 0;
            break;
        } else if (bufferPosition > 0 && incomingByte == '\"') {
            Log.info("Double message incoming?");
            memcpy(message, buffer, bufferPosition);
            message[bufferPosition] = '\0';
            messageReady = true;
            buffer[0] = incomingByte;
            bufferPosition = 1;
            messageStart = millis();
            break;
        } else if (incomingByte != 10 && incomingByte != 13) {
            buffer[bufferPosition++] = incomingByte;
        } else if (bufferPosition > 0) {
            memcpy(message, buffer, bufferPosition);
            message[bufferPosition] = '\0';
            messageReady = true;
            screenRequestRetryCount = 0;
            bufferPosition = 0;
            break;
        }
    }

    if (bufferPosition > 0 && millis() > (messageStart+100)) {
        Log.info("Message failed to receive within 100ms");
        memcpy(message, buffer, bufferPosition);
        message[bufferPosition] = '\0';
        messageReady = true;
        messageComplete = false;
        bufferPosition = 0;
    }

    if (messageReady) {
        char messageLength = strlen(message);
        Log.info(message);
        
        // Zone state changed
        if (messageLength == 6 &&
            strncmp(message, msgZoneUpdate, strlen(msgZoneUpdate)) == 0) {
            updateZoneState(message);
        // System Armed
        } else if (messageLength >= 6 &&
                    strncmp(message, msgArmUpdate, strlen(msgArmUpdate)) == 0) {
        // System Disarmed
        } else if (messageLength >= 6 &&
                    strncmp(message, msgDisarmUpdate, strlen(msgDisarmUpdate)) == 0) {
            if (currentTask != CRESTRON_IDLE)
                processTask(IS_DISARMED);
        // Entry while armed
        } else if (messageLength == 6 &&
                    strncmp(message, msgEntryUpdate, strlen(msgEntryUpdate)) == 0) {
        // System arming
        } else if (messageLength == 6 &&
                    strncmp(message, msgArmingUpdate, strlen(msgArmingUpdate)) == 0) {
            if (currentTask != CRESTRON_IDLE)
                processTask(IS_ARMING);
        // Intruder
        } else if (messageLength == 6 &&
                    strncmp(message, msgIntruderUpdate, strlen(msgIntruderUpdate)) == 0) {
        // User logged in with code or tag
        } else if (messageLength == 6 &&
                    (strncmp(message, msgUserPinLogin, strlen(msgUserPinLogin)) == 0 ||
                    strncmp(message, msgUserTagLogin, strlen(msgUserTagLogin)) == 0)) {
            int user = message[4] - '0';

            if (user < userCount)
                Log.info("User logged in: %s", users[user]);
            else
                Log.info("User logged in: Outside of user array size");

            if (currentTask != CRESTRON_IDLE)
                processTask(LOGIN_CONFIRMED);
        // Reply to ASTATUS request that the system is disarmed
        } else if (messageLength == 5 &&
                    strncmp(message, msgReplyDisarmed, strlen(msgReplyDisarmed)) == 0) {
            if (currentTask != CRESTRON_IDLE)
                processTask(IS_DISARMED);        
        // Reply to ASTATUS request that the system is armed
        } else if (messageLength == 5 &&
                    strncmp(message, msgReplyArmed, strlen(msgReplyArmed)) == 0) {
            if (currentTask != CRESTRON_IDLE) {
                processTask(IS_ARMED);
            }
        } else if (
                (messageLength >= strlen(msgScreenArmedPart) &&
                    strncmp(message, msgScreenArmedPart, strlen(msgScreenArmedPart)) == 0) ||
                (messageLength >= strlen(msgScreenArmedNight) &&
                    strncmp(message, msgScreenArmedNight, strlen(msgScreenArmedNight)) == 0) ||
                (messageLength >= strlen(msgScreenIdlePartArmed) &&
                    strncmp(message, msgScreenIdlePartArmed, strlen(msgScreenIdlePartArmed)) == 0)) {
            if (currentTask != CRESTRON_IDLE)
                processTask(SCREEN_PART_ARMED);
        } else if (messageLength >= strlen(msgScreenArmedFull) &&
                    strncmp(message, msgScreenArmedFull, strlen(msgScreenArmedFull)) == 0) {
            if (currentTask != CRESTRON_IDLE)
                processTask(SCREEN_FULL_ARMED);
        } else if (messageLength >= strlen(msgScreenIdle) &&
                    strncmp(message, msgScreenIdle, strlen(msgScreenIdle)) == 0) {
            if (currentTask != CRESTRON_IDLE)
                processTask(SCREEN_IDLE);
        // Shown directly after user logs in
        } else if (messageLength > strlen(msgWelcomeBack) &&
                    strncmp(message, msgWelcomeBack, strlen(msgWelcomeBack)) == 0) {
            if (currentTask == CRESTRON_WAIT_FOR_DISARM_PROMPT ||
                    currentTask == CRESTRON_WAIT_FOR_ARM_PROMPT)
                delayCommand(COMMAND_SCREEN_STATE, 500);
        // Shown shortly after user logs in
        } else if (messageLength >= strlen(msgScreenQuestionArm) &&
                    strncmp(message, msgScreenQuestionArm, strlen(msgScreenQuestionArm)) == 0) {
            if (currentTask != CRESTRON_IDLE)
                processTask(FULL_ARM_PROMPT);
        } else if (messageLength >= strlen(msgScreenQuestionPartArm) &&
                    strncmp(message, msgScreenQuestionPartArm, strlen(msgScreenQuestionPartArm)) == 0) {
            if (currentTask != CRESTRON_IDLE)
                processTask(PART_ARM_PROMPT);
        } else if (messageLength >= strlen(msgScreenQuestionNightArm) &&
                    strncmp(message, msgScreenQuestionNightArm, strlen(msgScreenQuestionNightArm)) == 0) {
            if (currentTask != CRESTRON_IDLE)
                processTask(NIGHT_ARM_PROMPT);
        } else if (messageLength >= strlen(msgScreenQuestionDisarm) &&
                    strncmp(message, msgScreenQuestionDisarm, strlen(msgScreenQuestionDisarm)) == 0) {
            if (currentTask != CRESTRON_IDLE)
                processTask(DISARM_PROMPT);
        } else if (messageLength >= strlen(msgScreenAreainEntry) &&
                    strncmp(message, msgScreenAreainEntry, strlen(msgScreenAreainEntry)) == 0) {
            if (currentTask != CRESTRON_IDLE)
                processTask(SCREEN_AREA_ENTRY);
        } else if (messageLength >= strlen(msgScreenAreainExit) &&
            strncmp(message, msgScreenAreainExit, strlen(msgScreenAreainExit)) == 0) {
            if (currentTask != CRESTRON_IDLE)
                processTask(SCREEN_AREA_EXIT);
        // Fails to arm. e.g.
        // Zone 012 Active Garage Door
        } else if (messageLength >= 17 &&
                    strncmp(message, "Zone", 4) == 0 &&
                    (strstr(message, "Active") - message) == 9) {
            Log.info("Failed to arm. Zone active while Arming");
            requestArmState();
        } else if (strcmp(message, "OK") == 0) {
            simpleProtocolTimeout = millis() + 30000;
            if (simpleTask == SIMPLE_LOGIN) {
                Log.info("Simple protocol login confirmed");
                simpleTask = SIMPLE_IDLE;
                activeProtocol = SIMPLE;
            } else if (simpleTask == SIMPLE_LOGOUT) {
                Log.info("Simple protocol logout confirmed");
                simpleTask = SIMPLE_IDLE;
                activeProtocol = CRESTRON;
            }
        } else if (strcmp(message, "ERROR") == 0) {
            if (simpleTask == SIMPLE_LOGIN) {
                Log.info("Simple login failed");
                simpleTask = SIMPLE_IDLE;
            }
        } else {
            if (message[0] == '"') {
                Log.info(String::format("Unknown Crestron command - %s", message));
            } else {
                Log.info("Unknown non-Crestron command - %s", message);
                if (message[0] < 64 || message[0] > 126) {
                    for (int i = 0; i < strlen(message); i++) {
                        Log.info("%d\n", message[i]);
                    }
                }
            }

            if (currentTask != CRESTRON_IDLE && !messageComplete) {
                if (screenRequestRetryCount++ < 3) {
                    if (currentTask == CRESTRON_CONFIRM_ARMED || currentTask == CRESTRON_CONFIRM_DISARMED) {
                        Log.info("Retrying arm state request");
                        requestArmState();
                    } else if (currentTask == CRESTRON_CONFIRM_IDLE_SCREEN ||
                                currentTask == CRESTRON_WAIT_FOR_ARM_PROMPT ||
                                currentTask == CRESTRON_WAIT_FOR_DISARM_PROMPT ||
                                currentTask == CRESTRON_WAIT_FOR_PART_ARM_PROMPT ||
                                currentTask == CRESTRON_WAIT_FOR_NIGHT_ARM_PROMPT) {
                        Log.info("Retrying screen request");
                        requestScreen();
                    } else {
                        Log.info("Retry count exceeded");
                    }
                }
            } else {
                processTask(UNKNOWN_MESSAGE);
            }
        }
    }

    if (performLogin && millis() > nextPinEntryTime) {
        texSerial.print("KEY");
        texSerial.println(userPin[loginPinPosition++]);

        if (loginPinPosition >= strlen(userPin)) {
            loginPinPosition = 0;
            nextPinEntryTime = 0;
            performLogin = false;
            if (currentTask != CRESTRON_IDLE) {
                processTask(LOGIN_COMPLETE);
            }
        } else {
            nextPinEntryTime = millis() + PIN_ENTRY_DELAY;
        }
    }

    if (currentTask == CRESTRON_IDLE && alarmState == ARMING &&
        millis() > (lastStateChange + armingTimeout)) {
        Log.info("Arming state timed out. Requesting arm state");
        lastStateChange = millis();
        requestArmState();
    }

    // used to execute delayed commands
    if (delayedCommandExecuteTime > 0 &&
        millis() > delayedCommandExecuteTime) {
        request(delayedCommand);
        delayedCommandExecuteTime = 0;
    }

    if (armStartTime != 0 &&
        millis() > (armStartTime + armTimeout))
        processTask(TASK_TIMEOUT);

    if (disarmStartTime != 0 &&
        millis() > (disarmStartTime + disarmTimeout)) {
        processTask(TASK_TIMEOUT);
    }

    if ((currentTask == CRESTRON_CONFIRM_IDLE_SCREEN || currentTask == CRESTRON_WAIT_FOR_ARM_PROMPT ||
        currentTask == CRESTRON_WAIT_FOR_DISARM_PROMPT || currentTask == CRESTRON_WAIT_FOR_PART_ARM_PROMPT ||
        currentTask == CRESTRON_WAIT_FOR_NIGHT_ARM_PROMPT)
        && bufferPosition == 0 && delayedCommandExecuteTime == 0 && millis() > (lastCommandTime+commandWaitTimeout) && commandAttempts < maxRetries) {
        Log.info("commandWaitTimeout: Retrying request screen");
        commandAttempts++;
        requestScreen();
    }

    // Switch to the Simple Protocol
    if (simpleTask == SIMPLE_LOGIN && millis() > (simpleCommandLastSent+500)) {
        Log.info("Performing simple login");
        simpleCommandLastSent = millis();
        texSerial.print("\\W");
        texSerial.print(udlCode);
        texSerial.print("/");
    }

    // Auto-logout of the Simple Protocol. Should never be required.
    if (millis() > simpleProtocolTimeout && activeProtocol == SIMPLE && simpleTask == SIMPLE_IDLE) {
        simpleTask = SIMPLE_LOGOUT;
        texSerial.print("\\H/");
        Log.info("Simple Protocol timeout");
    }

    checkDigiOutputs();
}