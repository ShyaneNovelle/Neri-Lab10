#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <esp_arduino_version.h>
#include <string.h>

// Change this on EACH student's board.
// Use lastname_firstname. Maximum: 31 characters.
const char OWNER_NAME[] = "jesser_neri";

// Everyone in the block must use the SAME channel.
const uint8_t RADIO_CHANNEL = 1;

// Everyone must keep this identification value unchanged.
const uint32_t MAGIC = 0xB10C2026;

static_assert(sizeof(OWNER_NAME) <= 32,
              "Owner name must be 31 characters or fewer.");

// Sends to listening ESP-NOW boards in range.
const uint8_t BROADCAST_MAC[6] = {
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

// Everyone must use this exact message layout.
// Each board fills owner with its own student's name.
struct Message {
  uint32_t magic;
  uint32_t counter;
  char owner[32];
};

static_assert(sizeof(Message) == 40,
              "The message must remain 40 bytes.");

// Stores a board's actual source MAC and its advertised name.
struct BoardRecord {
  uint8_t mac[6];
  char owner[32];
};

const size_t MAX_BOARDS = 100;

BoardRecord heardBoards[MAX_BOARDS] = {};
size_t heardCount = 0;
bool listFull = false;

uint8_t myMac[6] = {};
Message outgoing = {};

// Protects the list shared by the callback and loop().
// We do not create our own tasks or queues.
portMUX_TYPE boardListLock = portMUX_INITIALIZER_UNLOCKED;

unsigned long lastSend = 0;
unsigned long sendInterval = 1000;
unsigned long lastSummary = 0;


// Print a MAC address as AA:BB:CC:DD:EE:FF.
void printMac(const uint8_t* mac) {
  Serial.printf(
    "%02X:%02X:%02X:%02X:%02X:%02X",
    (unsigned)mac[0], (unsigned)mac[1],
    (unsigned)mac[2], (unsigned)mac[3],
    (unsigned)mac[4], (unsigned)mac[5]
  );
}


// Stop if a required setup step fails.
void stopWithError(const char* message) {
  Serial.println(message);

  while (true) {
    delay(1000);
  }
}


// Copy the received name safely.
// Ignore control characters that could disturb Serial Monitor.
void copyOwnerName(char* destination, const char* source) {
  for (size_t i = 0; i < 31; i++) {
    unsigned char character = (unsigned char)source[i];

    if (character == '\0') {
      destination[i] = '\0';
      return;
    }

    destination[i] =
      (character >= 32 && character <= 126)
      ? (char)character
      : '?';
  }

  destination[31] = '\0';
}


// Check an incoming message and record its sender.
void handleMessage(const uint8_t* mac,
                   const uint8_t* data,
                   int length) {
  if (mac == nullptr || data == nullptr) {
    return;
  }

  // Reject messages using the old layout or another size.
  if (length != (int)sizeof(Message)) {
    return;
  }

  Message incoming = {};
  memcpy(&incoming, data, sizeof(incoming));

  if (incoming.magic != MAGIC) {
    return;
  }

  // Never include our own board in the count.
  if (memcmp(mac, myMac, 6) == 0) {
    return;
  }

  BoardRecord received = {};

  // Use the actual sender MAC from ESP-NOW.
  // We do not trust a MAC typed into someone's message.
  memcpy(received.mac, mac, 6);
  copyOwnerName(received.owner, incoming.owner);

  if (received.owner[0] == '\0') {
    strcpy(received.owner, "unnamed");
  }

  portENTER_CRITICAL(&boardListLock);

  size_t existingIndex = heardCount;

  // Identify boards by MAC, not by owner name.
  for (size_t i = 0; i < heardCount; i++) {
    if (memcmp(heardBoards[i].mac, received.mac, 6) == 0) {
      existingIndex = i;
      break;
    }
  }

  if (existingIndex < heardCount) {
    // Same board: refresh its name without increasing the count.
    memcpy(heardBoards[existingIndex].owner,
           received.owner,
           sizeof(received.owner));
  } else if (heardCount < MAX_BOARDS) {
    // New MAC: add one new board.
    heardBoards[heardCount] = received;
    heardCount++;
  } else {
    listFull = true;
  }

  portEXIT_CRITICAL(&boardListLock);

  // Printing happens outside this callback.
}


// Receive callback for Arduino ESP32 core 3.x.
#if ESP_ARDUINO_VERSION_MAJOR >= 3

void onReceive(const esp_now_recv_info_t* info,
               const uint8_t* data,
               int length) {
  if (info != nullptr) {
    handleMessage(info->src_addr, data, length);
  }
}

// Receive callback for Arduino ESP32 core 2.x.
#else

void onReceive(const uint8_t* mac,
               const uint8_t* data,
               int length) {
  handleMessage(mac, data, length);
}

#endif


// Print a consistent copy of the results.
void printSummary() {
  // Static keeps this larger array off the loop's stack.
  static BoardRecord snapshot[MAX_BOARDS];

  size_t count;
  bool full;

  portENTER_CRITICAL(&boardListLock);

  count = heardCount;
  full = listFull;
  memcpy(snapshot, heardBoards, count * sizeof(BoardRecord));

  portEXIT_CRITICAL(&boardListLock);

  Serial.println("\n========== ESP-NOW RESULTS ==========");

  Serial.print("My name      : ");
  Serial.println(OWNER_NAME);

  Serial.print("My board MAC : ");
  printMac(myMac);
  Serial.println();

  Serial.printf("Radio channel: %u\n",
                (unsigned)RADIO_CHANNEL);

  Serial.printf("Running for  : %lu seconds\n",
                millis() / 1000UL);

  Serial.printf("Boards heard : %u\n",
                (unsigned)count);

  Serial.println("Owner / MAC address:");

  if (count == 0) {
    Serial.println("  None yet.");
  }

  for (size_t i = 0; i < count; i++) {
    Serial.printf("  %u. %s | ",
                  (unsigned)(i + 1),
                  snapshot[i].owner);

    printMac(snapshot[i].mac);
    Serial.println();
  }

  if (full) {
    Serial.println("WARNING: Board list full.");
    Serial.println("The actual number may exceed the stored count.");
  }

  Serial.println("=====================================\n");
}


void setup() {
  Serial.begin(115200);
  delay(1000);

  // Enable the radio without joining a router or hotspot.
  if (!WiFi.mode(WIFI_STA)) {
    stopWithError("Could not start the radio.");
  }

  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false, false);
  delay(100);

  // All participating boards must use the same channel.
  if (esp_wifi_set_channel(
        RADIO_CHANNEL, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
    stopWithError("Could not set the radio channel.");
  }

  // Read the MAC of this physical board.
  if (esp_wifi_get_mac(WIFI_IF_STA, myMac) != ESP_OK) {
    stopWithError("Could not read this board's MAC.");
  }

  // Prepare our message.
  outgoing.magic = MAGIC;
  outgoing.counter = 0;
  strcpy(outgoing.owner, OWNER_NAME);

  if (esp_now_init() != ESP_OK) {
    stopWithError("ESP-NOW initialization failed.");
  }

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST_MAC, 6);
  peer.channel = RADIO_CHANNEL;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;

  if (esp_now_add_peer(&peer) != ESP_OK) {
    stopWithError("Could not add the broadcast peer.");
  }

  if (esp_now_register_recv_cb(onReceive) != ESP_OK) {
    stopWithError("Could not register the receive callback.");
  }

  Serial.println("ESP-NOW ready.");
  Serial.println("No router or hotspot connection is used.");
  Serial.println("Names are entered by each board's owner.");
  Serial.println("Count includes unique OTHER boards since startup.");
  Serial.println("Wait at least 30 seconds during the block window.");

  printSummary();
}


void loop() {
  unsigned long now = millis();

  // Broadcast our name approximately once per second.
  if (now - lastSend >= sendInterval) {
    lastSend = now;
    sendInterval = 1000 + (esp_random() % 301);

    outgoing.counter++;

    esp_err_t result = esp_now_send(
      BROADCAST_MAC,
      reinterpret_cast<const uint8_t*>(&outgoing),
      sizeof(outgoing)
    );

    // Accepted for sending does not prove another board received it.
    if (result != ESP_OK) {
      Serial.printf("Send request failed: %d\n", (int)result);
    }
  }

  // Repeat all names, MAC addresses, and the count every five seconds.
  if (now - lastSummary >= 5000) {
    lastSummary = now;
    printSummary();
  }

  delay(5);
}