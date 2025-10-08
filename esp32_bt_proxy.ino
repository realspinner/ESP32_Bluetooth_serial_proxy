/**
 * @file esp32_bt_proxy.ino
 * @brief Прошивка для ESP32, реализующая прокси-мост между UART и Bluetooth SPP.
 *
 * Эта программа позволяет ESP32 работать в качестве моста, перенаправляя данные
 * из последовательного порта (UART) на удаленное Bluetooth-устройство и обратно.
 * Устройство поддерживает командный режим для настройки параметров подключения,
 * таких как MAC-адрес и PIN-код удаленного устройства.
 */

#include <BluetoothSerial.h>
#include <Preferences.h>
#include <map>

// Версия прошивки
#define FIRMWARE_VERSION "1.0"

// Префикс, которым будут предваряться все системные сообщения
#define SYSPREFIX "%ESP32_BTP% "


// Проверяем, что классический BT доступен
#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED)
#error Bluetooth is not enabled! Please run ` make menuconfig` to and enable it
#endif

#if !defined(CONFIG_BT_SPP_ENABLED)
#error Serial Bluetooth not available or not enabled. It is only available for the ESP32 chip.
#endif

// Включаем поддержку классического Bluetooth SPP
BluetoothSerial SerialBT;
bool confirmRequestDone = false;

// MAC-адрес удалённого устройства по умолчанию
const char *DEFAULT_REMOTE_ADDR = "FF:FF:FF:FF:FF:FF";
// Пин-код удаленного устройства по умолчанию
const char *DEFAULT_REMOTE_PIN = "0000";
bool lastBtnState = false; // Предыдущее состояние кнопки
bool commandMode = false;  // Флаг командного режима
bool connectionGood = false; // Флаг успешного соединения

/**
 * @struct BindRecord_t
 * @brief Структура для хранения данных о сопряжении с удаленным устройством.
 */
typedef struct {
    bool valid = false;       ///< Флаг, указывающий на валидность записи
    esp_bd_addr_t remoteAddr; ///< MAC-адрес удаленного устройства
    char remotePin[9]{};      ///< PIN-код удаленного устройства
} BindRecord_t;

BTAddress addr = BTAddress(DEFAULT_REMOTE_ADDR);
char pin[9] = {'0', '0', '0', '0', 0x00, 0x00, 0x00, 0x00, 0x00};
const int channel = 1; // RFCOMM канал для SPP

Preferences prefs;   // Объект для работы с энергонезависимой памятью
BindRecord_t bindRec{}; // Запись с данными для сопряжения

// Маска безопасности для SPP: шифрование и аутентификация
esp_spp_sec_t sec_mask =
        ESP_SPP_SEC_ENCRYPT |
        ESP_SPP_SEC_AUTHENTICATE;
// Роль устройства в SPP соединении (SLAVE)
esp_spp_role_t role = ESP_SPP_ROLE_SLAVE;

constexpr size_t LINE_BUFFER_LENGTH = 64; // Длина буфера для команд
char lineBuffer[LINE_BUFFER_LENGTH + 1]{}; // Буфер для команд из UART
size_t linePos = 0; // Текущая позиция в буфере команд

constexpr int BT_DISCOVER_TIME = 10000; // Время сканирования Bluetooth в мс

// Буферы для пересылки данных
static uint8_t btBuffer[256]{};
static size_t btBufferLen{};

static uint8_t uartBuffer[256]{};
static size_t uartBufferLen{};

/**
 * @brief Проверяет состояние кнопки для переключения в командный режим.
 *
 * Считывает состояние пина GPIO 0. При нажатии на кнопку (уровень LOW)
 * инвертирует флаг `commandMode` и сообщает о смене режима в Serial.
 */
void checkButton() {
    // Считываем текущее состояние кнопки (LOW, если нажата)
    bool btnState = (digitalRead(0) == LOW);
    // Проверяем, изменилось ли состояние с последнего считывания
    if (btnState != lastBtnState) {
        // Если кнопка была нажата
        if (btnState) {
            // Инвертируем режим (вход/выход из командного режима)
            commandMode = !commandMode;
            // Сообщаем о смене режима
            reportCommandMode();
        }
        // Сохраняем текущее состояние кнопки
        lastBtnState = btnState;
    }
}

/**
 * @brief Загружает данные о сопряжении из энергонезависимой памяти.
 *
 * Открывает пространство "BTPROXY" в NVS и считывает MAC-адрес и PIN-код
 * удаленного устройства. Если данные найдены и корректны, устанавливает
 * флаг `bindRec.valid` в `true`.
 */
void loadBindRecord() {
    // Открываем хранилище в режиме "только для чтения"
    prefs.begin("BTPROXY", true);
    bindRec.valid = false;
    // Проверяем наличие ключа с адресом
    if (prefs.isKey("rAddr")) {
        // Считываем адрес
        prefs.getBytes("rAddr", bindRec.remoteAddr,
                       sizeof(BindRecord_t::remoteAddr));
    } else {
        // Если ключа нет, прекращаем загрузку
        return;
    }

    // Проверяем наличие ключа с PIN-кодом
    if (prefs.isKey("rPin")) {
        // Считываем PIN-код
        prefs.getBytes("rPin", bindRec.remotePin, sizeof(BindRecord_t::remotePin));
    } else {
        // Если ключа нет, прекращаем загрузку
        return;
    }

    // Если все данные успешно загружены, помечаем запись как валидную
    bindRec.valid = true;

    // Переносим загруженные значения в рабочие переменные
    addr = BTAddress(bindRec.remoteAddr);
    memcpy(pin, bindRec.remotePin, sizeof(pin));
}


/**
 * @brief Сохраняет данные о сопряжении в энергонезависимую память.
 *
 * Если запись `bindRec` валидна, функция очищает старые настройки
 * и записывает новый MAC-адрес и PIN-код в пространство "BTPROXY".
 * Также выполняет проверку корректности записи.
 */
void saveBindRecord() {
    // Не сохраняем, если запись невалидна
    if (!bindRec.valid) {
        return;
    }

    BindRecord_t bindCheck{}; // Структура для проверки записи

    // Открываем хранилище для записи
    prefs.begin("BTPROXY", false);
    // Очищаем предыдущие настройки
    prefs.clear();
    prefs.end();

    // Открываем хранилище снова для записи новых данных
    prefs.begin("BTPROXY", false);
    // Записываем MAC-адрес
    prefs.putBytes("rAddr", bindRec.remoteAddr, sizeof(BindRecord_t::remoteAddr));
    // Записываем PIN-код
    prefs.putBytes("rPin", bindRec.remotePin, sizeof(BindRecord_t::remotePin));

    // Считываем только что записанные данные для проверки
    prefs.getBytes("rAddr", bindCheck.remoteAddr,
                   sizeof(BindRecord_t::remoteAddr));
    prefs.getBytes("rPin", bindCheck.remotePin, sizeof(BindRecord_t::remotePin));

    // Сравниваем записанный адрес с оригиналом
    for (int i = 0; i < sizeof(BindRecord_t::remoteAddr); i++) {
        if (bindCheck.remoteAddr[i] != bindRec.remoteAddr[i]) {
            Serial.println(SYSPREFIX "Error: remote address not saved");
            break;
        }
    }

    // Сравниваем записанный PIN-код с оригиналом
    for (int i = 0; i < sizeof(BindRecord_t::remoteAddr); i++) {
        if (bindCheck.remotePin[i] != bindRec.remotePin[i]) {
            Serial.println(SYSPREFIX "ERROR: remote pin is not saved");
            break;
        }
    }

    // Закрываем хранилище
    prefs.end();
    Serial.println(SYSPREFIX "DEBUG: Preferences saved");
}

/**
 * @brief Устанавливает данные о сопряжении по умолчанию.
 *
 * Заполняет структуру `bindRec` значениями `DEFAULT_REMOTE_ADDR`
 * и `DEFAULT_REMOTE_PIN`.
 */
void defaultBindRecord() {
    // Устанавливаем MAC-адрес по умолчанию
    BTAddress a = BTAddress(DEFAULT_REMOTE_ADDR);
    memcpy(bindRec.remoteAddr, a.getNative(), sizeof(BindRecord_t::remoteAddr));
    // Устанавливаем PIN-код по умолчанию
    memcpy(bindRec.remotePin, DEFAULT_REMOTE_PIN,
           sizeof(BindRecord_t::remotePin));
    // Помечаем запись как валидную
    bindRec.valid = true;
    Serial.println(SYSPREFIX "DEBUG: default preferences loaded");
}

/**
 * @brief Сообщает о текущем состоянии командного режима.
 *
 * Выводит в Serial информацию о том, включен или выключен командный режим.
 */
void reportCommandMode() {
    Serial.print(SYSPREFIX "CMD MODE: ");
    Serial.println((commandMode) ? "ENABLED" : "DISABLED");
}

/**
 * @brief Начальная настройка устройства.
 *
 * Инициализирует Serial, настраивает пин кнопки, загружает настройки
 * сопряжения (или использует значения по умолчанию), инициализирует Bluetooth
 * и пытается установить соединение с удаленным устройством.
 */
void setup() {
    // Настраиваем пин кнопки на ввод с подтяжкой к питанию
    pinMode(0, INPUT_PULLUP);

    // Инициализируем последовательный порт
    Serial.begin(115200);
    Serial.println(SYSPREFIX "INFO: ESP32-BT-PROXY");
    Serial.println(SYSPREFIX "INFO: firmware " FIRMWARE_VERSION);

    // Загружаем настройки сопряжения
    Serial.println(SYSPREFIX "DEBUG: loading preferences");
    loadBindRecord();
    // Если валидные настройки не найдены
    if (!bindRec.valid) {
        Serial.println(SYSPREFIX
            "DEBUG: no valid preferences found, using defaults");
        // Используем и сохраняем настройки по умолчанию
        defaultBindRecord();
        saveBindRecord();
    }
    Serial.print(SYSPREFIX "INFO: Remote address ");
    Serial.println(addr.toString().c_str());
    Serial.print(SYSPREFIX "INFO: Remote pin ");
    Serial.println(pin);

    // Инициализируем Bluetooth с именем "ESP32_BTPROXY"
    if (!SerialBT.begin("ESP32_BTPROXY", true)) {
        Serial.println(SYSPREFIX
            "ERROR: Bluetooth initialization failed! Can't go on");
        while (true);
    }

    Serial.println(SYSPREFIX "STATE: DISCONNECTED");

    // Цикл попыток подключения
    while (!SerialBT.connected()) {
        // Устанавливаем PIN-код для подключения
        size_t pinSize = strlen(pin);
        SerialBT.setPin(pin, pinSize);
        Serial.print(SYSPREFIX "DEBUG: Trying to connect ");
        Serial.print(addr.toString().c_str());
        Serial.println("...");
        // Пытаемся подключиться как master
        if (SerialBT.connect(addr, channel, sec_mask, role)) {
            // Если `connect` вернул true, выходим из цикла
            break;
        } else {
            // Если `connect` вернул false, ждем до 10 секунд
            if (!SerialBT.connected(10000)) {
                Serial.println(SYSPREFIX "ERROR: Connection failed");
                SerialBT.disconnect();
                Serial.println(SYSPREFIX "STATE: DISCONNECTED");
            } else {
                // Если за это время подключились, выходим
                break;
            }
        }

        // Даем пользователю шанс войти в командный режим
        Serial.println(SYSPREFIX "INFO: Press the \"boot\" button to enter command mode");
        for (int i = 0; i < 100; i++) {
            delay(10);
            checkButton();
            if (commandMode) {
                // Если вошли в командный режим, выходим из setup для обработки команд в loop
                return;
            }
        }
    }

    // Проверяем статус соединения после цикла
    if (SerialBT.connected(1000)) {
        reportCommandMode();
        Serial.println(SYSPREFIX "STATE: CONNECTED");
    }
}

/**
 * @brief Выводит справку по доступным командам.
 */
void printCmdHelp() {
    Serial.println(SYSPREFIX "HELP: List of available commands:");
    Serial.println(SYSPREFIX "HELP:   HELP - prints this information");
    Serial.println(
        SYSPREFIX
        "HELP:   SCAN - scan nearby BT devices and display addresses and names");
    Serial.println(
        SYSPREFIX
        "HELP:   SETADDR XX:XX:XX:XX:XX:XX - set address of the remote device");
    Serial.println(SYSPREFIX
        "HELP:   SETPIN XXXX - set pin code of the remoted device");
    Serial.println(SYSPREFIX "HELP:   INFO - display configuration");
    Serial.println(SYSPREFIX "HELP:   CLEAR - remove configuration");
}

/**
 * @brief Выводит информацию о текущей конфигурации.
 *
 * Отображает сохраненный MAC-адрес и PIN-код удаленного устройства.
 */
void printInfo() {
    BTAddress a = BTAddress(bindRec.remoteAddr);

    Serial.print(SYSPREFIX "INFO: Remote address ");
    Serial.println(a.toString().c_str());
    Serial.print(SYSPREFIX "INFO: PIN ");
    Serial.println(bindRec.remotePin);
}

/**
 * @brief Выполняет асинхронное сканирование Bluetooth-устройств.
 *
 * Отключается от текущего устройства (если подключено), запускает сканирование
 * на `BT_DISCOVER_TIME` миллисекунд. Для каждого найденного устройства выводит
 * его адрес, имя, RSSI и доступные SPP-сервисы.
 */
void asyncScan() {
    SerialBT.disconnect();

    Serial.println(SYSPREFIX "INFO: Scanning, please wait...");
    BTScanResults *btDeviceList =
            SerialBT.getScanResults();
    // Запускаем асинхронное сканирование
    if (SerialBT.discoverAsync([](BTAdvertisedDevice *pDevice) {
        // Этот callback вызывается для каждого найденного устройства
        Serial.print(SYSPREFIX);
        Serial.printf(" SCAN: found %s\n", pDevice->toString().c_str());
    })) {
        // Ждем завершения сканирования
        delay(BT_DISCOVER_TIME);
        Serial.println(SYSPREFIX "INFO: Stopping...");
        SerialBT.discoverAsyncStop();
        Serial.println(SYSPREFIX "INFO: Scan services...");
        delay(5000); // Дополнительная пауза для опроса доступных сервисов
        if (btDeviceList->getCount() > 0) {
            BTAddress addr;
            int channel = 0;
            Serial.println(SYSPREFIX "INFO: Found devices:");
            for (int i = 0; i < btDeviceList->getCount(); i++) {
                BTAdvertisedDevice *device = btDeviceList->getDevice(i);
                // Выводим основную информацию об устройстве
                Serial.print(SYSPREFIX);
                Serial.printf("INFO: Address: %s  Name: [%s]  RSSI: %d\n",
                              device->getAddress().toString().c_str(),
                              device->getName().c_str(), device->getRSSI());
                // Получаем список каналов (сервисов) для данного устройства
                std::map<int, std::string> channels =
                        SerialBT.getChannels(device->getAddress());
                Serial.print(SYSPREFIX);
                Serial.printf("INFO:        Services found: %d\n", channels.size());
                // Выводим информацию о каждом сервисе
                for (auto const &entry: channels) {
                    Serial.printf("INFO:        channel %d (%s)\n", entry.first,
                                  entry.second.c_str());
                }
            }
        } else {
            Serial.println(SYSPREFIX "INFO: no devices found");
        }
    } else {
        Serial.println(
            SYSPREFIX
            "ERROR: Can't run discovery, is the remote connected already?");
    }
    Serial.println(SYSPREFIX "INFO: Scan complete");
}

/**
 * @brief Сбрасывает буфер для ввода команд.
 *
 * Заполняет `lineBuffer` нулями и сбрасывает указатель `linePos`.
 */
void resetLine() {
    memset(lineBuffer, 0x00, sizeof(lineBuffer));
    linePos = 0;
}

/**
 * @brief Обрабатывает входящие символы из Serial в командном режиме.
 * @param c Символ для обработки.
 *
 * Собирает символы в `lineBuffer`. При получении символа новой строки `\n`
 * или при переполнении буфера вызывает `parseCommand` для обработки команды.
 */
void processInput(char c) {
    // Игнорируем символ возврата каретки
    if (c == '\r')
        return;
    // Если пришел символ новой строки, обрабатываем команду
    if (c == '\n') {
        parseCommand();
        resetLine();
        return;
    }

    // Добавляем символ в буфер
    lineBuffer[linePos] = c;
    ++linePos;
    // Если буфер заполнен, обрабатываем команду
    if (linePos >= LINE_BUFFER_LENGTH) {
        parseCommand();
        resetLine();
    }
}

/**
 * @brief Проверяет корректность MAC-адреса.
 * @param arg Строка, содержащая MAC-адрес для проверки.
 * @return `true`, если адрес корректен, иначе `false`.
 *
 * Сравнивает исходную строку (приведенную к нижнему регистру) с результатом
 * преобразования этой строки в объект `BTAddress` и обратно в строку.
 */
bool validateAddr(const char *arg) {
    String newAddr = arg;
    // Приводим все символы к нижнему регистру
    for (auto &c: newAddr)
        c = tolower(c);
    // Создаем объект BTAddress для проверки
    BTAddress check = BTAddress(newAddr);
    String checkStr = check.toString();
    // Приводим проверочную строку также к нижнему регистру
    for (auto &c: checkStr)
        c = tolower(c);
    // Сравниваем исходную и проверочную строки
    return checkStr == newAddr;
}

/**
 * @brief Устанавливает MAC-адрес удаленного устройства.
 * @param arg Строка с новым MAC-адресом.
 *
 * Проверяет корректность адреса, и в случае успеха, сохраняет его
 * в `bindRec`.
 */
void setBindAddress(char const *arg) {
    bindRec.valid = false;
    // Проверяем валидность адреса
    if (!validateAddr(arg)) {
        Serial.println(SYSPREFIX "ERROR: invalid address");
        return;
    }
    // Создаем объект BTAddress из строки
    BTAddress newAddr = BTAddress(arg);
    // Копируем "родной" формат адреса в нашу структуру
    memcpy(bindRec.remoteAddr, newAddr.getNative(),
           sizeof(BindRecord_t::remoteAddr));
    Serial.print(SYSPREFIX "INFO: New remote address is ");
    Serial.println(newAddr.toString().c_str());
    // Помечаем запись как валидную
    bindRec.valid = true;
}

/**
 * @brief Устанавливает PIN-код удаленного устройства.
 * @param arg Строка с новым PIN-кодом.
 *
 * Проверяет, что PIN-код не пустой и состоит только из цифр.
 * Сохраняет его в `bindRec`.
 */
void setBindPin(char const *arg) {
    bindRec.valid = false;
    size_t arglen = strlen(arg);
    size_t maxlen = sizeof(BindRecord_t::remotePin) - 1;

    // Проверяем, что PIN не пустой
    if (arglen == 0) {
        Serial.println(SYSPREFIX "ERROR: invalid pin");
        return;
    }

    // Проверяем, что все символы - цифры
    for (int i = 0; i < maxlen; i++) {
        if (arg[i] == 0x00) {
            break;
        }
        if ((arg[i] < '0') || (arg[i] > '9')) {
            Serial.println(SYSPREFIX
                "ERROR: invalid character in pin, only 0..9 allowed");
            return;
        }
    }

    // Очищаем буфер для PIN-кода
    memset(bindRec.remotePin, 0x00, maxlen + 1);
    // Копируем новый PIN, обрезая до максимальной длины
    memcpy(bindRec.remotePin, arg, (arglen < maxlen) ? arglen : maxlen);
    bindRec.valid = true;

    Serial.print(SYSPREFIX "INFO: New pin is ");
    Serial.println(bindRec.remotePin);
}

/**
 * @brief Разбирает и выполняет команду, введенную в Serial.
 *
 * Сравнивает введенную строку с известными командами (HELP, INFO, SCAN,
 * CLEAR, SETADDR, SETPIN) и вызывает соответствующие функции.
 */
void parseCommand() {
    char cmdUpper[8 + 1]{}; // Буфер для команды в верхнем регистре
    // Копируем начало строки команды и приводим к верхнему регистру
    for (int i = 0; i < (sizeof(cmdUpper) - 1); i++) {
        if (lineBuffer[i] == 0x00) {
            break;
        }
        cmdUpper[i] = toupper(lineBuffer[i]);
    }

    // Ищем и выполняем соответствующую команду
    if (strstr(cmdUpper, "HELP") == cmdUpper) {
        printCmdHelp();
        return;
    }

    if (strstr(cmdUpper, "INFO") == cmdUpper) {
        printInfo();
        return;
    }

    if (strstr(cmdUpper, "SCAN") == cmdUpper) {
        asyncScan();
        return;
    }

    if (strstr(cmdUpper, "CLEAR") == cmdUpper) {
        defaultBindRecord();
        saveBindRecord();
        return;
    }

    if (strstr(cmdUpper, "SETADDR") == cmdUpper) {
        // Передаем в функцию указатель на аргумент (после "SETADDR ")
        setBindAddress(lineBuffer + strlen("SETADDR "));
        if (bindRec.valid) {
            saveBindRecord();
        }
        return;
    }

    if (strstr(cmdUpper, "SETPIN") == cmdUpper) {
        // Передаем в функцию указатель на аргумент (после "SETPIN ")
        setBindPin(lineBuffer + strlen("SETPIN "));
        if (bindRec.valid) {
            saveBindRecord();
        }
        return;
    }

    // Если команда не распознана, выводим ошибку
    Serial.print(SYSPREFIX);
    Serial.printf("ERROR: unknown command %s, type HELP for help", lineBuffer);
}

/**
 * @brief Основной цикл программы.
 *
 * Проверяет состояние кнопки для смены режима.
 * В режиме прокси: перенаправляет данные между Serial и SerialBT.
 *   При потере соединения перезагружает устройство.
 * В командном режиме: считывает данные из Serial для обработки команд.
 */
void loop() {
    // Проверяем нажатие кнопки для смены режима
    checkButton();

    // Если мы не в командном режиме (режим прокси)
    if (!commandMode) {
        // Проверяем состояние соединения
        if (!SerialBT.connected()) {
            // Если соединение было ранее установлено, сообщаем о потере
            if (connectionGood) {
                Serial.println(SYSPREFIX);
                Serial.println(SYSPREFIX "ERROR: Connection lost");
                SerialBT.disconnect();
                Serial.println(SYSPREFIX "STATE: DISCONNECTED");
            }
            // Выполняем сброс программы для повторной попытки подключения
            Serial.println(SYSPREFIX "INFO: RESTART");
            ESP.restart();
        }

        // Перенаправляем данные из Bluetooth в Serial
        btBufferLen = 0;
        while (SerialBT.available()) {
            btBuffer[btBufferLen] = SerialBT.read();
            ++btBufferLen;
            // Если буфер полон, отправляем его в Serial
            if (btBufferLen >= sizeof(btBuffer)) {
                Serial.write(btBuffer, btBufferLen);
                btBufferLen = 0;
            }
        }
        // Отправляем остаток данных в буфере
        if (btBufferLen > 0) {
            Serial.write(btBuffer, btBufferLen);
        }

        // Перенаправляем данные из Serial в Bluetooth
        uartBufferLen = 0;
        while (Serial.available()) {
            uartBuffer[uartBufferLen] = Serial.read();
            ++uartBufferLen;
            // Если буфер полон, отправляем его в Bluetooth
            if (uartBufferLen >= sizeof(uartBuffer)) {
                SerialBT.write(uartBuffer, uartBufferLen);
                uartBufferLen = 0;
            }
        }
        // Отправляем остаток данных в буфере
        if (uartBufferLen > 0) {
            SerialBT.write(uartBuffer, uartBufferLen);
        }
    } else { // Если мы в командном режиме
        // Считываем символы из Serial и передаем их на обработку команд
        while (Serial.available()) {
            char c = Serial.read();
            processInput(c);
        }
    }
}
