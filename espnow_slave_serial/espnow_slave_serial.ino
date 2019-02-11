#include "NodeList.h"
#include <WifiEspNow.h>
extern "C" {
#include <espnow.h>
}
#if defined(ESP8266)
#include <ESP8266WiFi.h>
#elif defined(ESP32)
#include <WiFi.h>
#endif
#include "lib/cryptModule.h"
#include "lib/helperFunctions.h"
#include <CRC32.h>

#define BLUE_LED 2
#define RED_LED 16
bool flashRed = false;
bool flashBlue = false;

enum messageType_t {
	SENSOR_DATA = 0x01,
	CLIENT_HELLO = 0xFF,
	SERVER_HELLO = 0xFE,
	KEY_EXCHANGE_FINISHED = 0xFD,
	CYPHER_FINISHED = 0xFC,
	INVALIDATE_KEY = 0xFB
};

uint8_t myPublicKey[KEY_LENGTH];

node_t node;

NodeList nodelist;

bool serverHello (byte *key, Node *node) {
	uint8_t buffer[1 + IV_LENGTH + KEY_LENGTH + CRC_LENGTH];
	uint32_t crc32;

	if (!key) {
        DEBUG_ERROR ("NULL key");
		return false;
	}

	buffer[0] = SERVER_HELLO; // Server hello message
    
    CryptModule::random (buffer + 1, IV_LENGTH);

    DEBUG_VERBOSE ("IV: %s", printHexBuffer (buffer + 1, IV_LENGTH));

	for (int i = 0; i < KEY_LENGTH; i++) {
		buffer[1 + IV_LENGTH + i] = key[i];
	}

	crc32 = CRC32::calculate (buffer, 1 + IV_LENGTH + KEY_LENGTH);
	DEBUG_VERBOSE ("CRC32 = 0x%08X", crc32);

	// int is little indian mode on ESP platform
	uint8_t *crcField = buffer + 1 + IV_LENGTH + KEY_LENGTH;

    memcpy (crcField, &crc32, CRC_LENGTH);

    DEBUG_VERBOSE ("Server Hello message: %s", printHexBuffer (buffer, 1 + IV_LENGTH + KEY_LENGTH + CRC_LENGTH));
	flashRed = true;

#ifdef DEBUG_ESP_PORT
    char mac[18];
    mac2str (node->getMacAddress (), mac);
#endif
    DEBUG_INFO (" -------> SERVER_HELLO");
    if (esp_now_send (node->getMacAddress (), buffer, 1 + IV_LENGTH + KEY_LENGTH + CRC_LENGTH) == 0) {
        DEBUG_INFO ("Server Hello message sent to %s", mac);
        return true;
    } else {
        nodelist.unregisterNode (node);
        DEBUG_ERROR ("Error sending Server Hello message to %s", mac);
        return false;
    }
}

bool checkCRC (const uint8_t *buf, size_t count, uint32_t *crc) {
    uint32_t recvdCRC;

    memcpy (&recvdCRC, crc, sizeof (uint32_t)); // Use of memcpy is a must to ensure code does not try to read non memory aligned int
    uint32_t _crc = CRC32::calculate (buf, count);
    DEBUG_VERBOSE ("CRC32 =  Calc: 0x%08X Recvd: 0x%08X %s", _crc, recvdCRC, (_crc == recvdCRC) ? "OK" : "FAIL");
    return (_crc == recvdCRC);
}

bool processClientHello (const uint8_t mac[6], const uint8_t* buf, size_t count, Node *node) {
	//uint8_t myPublicKey[KEY_LENGTH];
    uint32_t crc32;

    if (count < 1 + IV_LENGTH + KEY_LENGTH + CRC_LENGTH) {
        DEBUG_WARN ("Message too short");
        return false;
    }

    memcpy (&crc32, buf + 1 + IV_LENGTH + KEY_LENGTH, CRC_LENGTH);

	if (!checkCRC (buf, 1 + IV_LENGTH + KEY_LENGTH, &crc32)) {
        DEBUG_WARN ("Wrong CRC");
		return false;
	}

    node->setLastMessageTime(millis ());
    node->setEncryptionKey (buf + 1 + IV_LENGTH);

    Crypto.getDH1 ();
	memcpy (myPublicKey, Crypto.getPubDHKey (), KEY_LENGTH);
    
    if (Crypto.getDH2 (node->getEncriptionKey ())) {
        node->setKeyValid (true);
        node->setStatus (INIT);
        DEBUG_INFO ("Node key: %s", printHexBuffer (node->getEncriptionKey (), KEY_LENGTH));
    } else {
        nodelist.unregisterNode (node);
        char macstr[18];
        mac2str ((uint8_t *)mac, macstr);
        DEBUG_ERROR ("DH2 error with %s", macstr);
        return false;
    }
}

bool cipherFinished (Node *node) {
    byte buffer[/*3 + RANDOM_LENGTH + CRC_LENGTH*/BLOCK_SIZE];
    uint32_t crc32;
    uint32_t nonce;

    memset (buffer, 0, BLOCK_SIZE);

    buffer[0] = CYPHER_FINISHED; // Server hello message

    uint16_t nodeId = node->getNodeId ();
    memcpy (buffer + 1, &nodeId, sizeof (uint16_t));
    
    nonce = Crypto.random ();

    memcpy (buffer + 3, &nonce, RANDOM_LENGTH);

    crc32 = CRC32::calculate (buffer, 3 + RANDOM_LENGTH);
    DEBUG_VERBOSE ("CRC32 = 0x%08X", crc32);

    // int is little indian mode on ESP platform
    uint32_t *crcField = (uint32_t*)&(buffer[3 + RANDOM_LENGTH]);
    *crcField = crc32;

    DEBUG_VERBOSE ("Cipher Finished message: %s", printHexBuffer (buffer, BLOCK_SIZE));

    Crypto.encryptBuffer (buffer, buffer, BLOCK_SIZE, node->getEncriptionKey());

    DEBUG_VERBOSE ("Encripted Cipher Finished message: %s", printHexBuffer (buffer, BLOCK_SIZE));

    flashRed = true;
    return WifiEspNow.send (node->getMacAddress(), buffer, BLOCK_SIZE);
}

bool processKeyExchangeFinished (const uint8_t mac[6], const uint8_t* buf, size_t count) {
    if (!checkCRC (buf, count - 4, (uint32_t*)(buf + count - 4))) {
        DEBUG_WARN ("Wrong CRC");
        return false;
    }
    //DEBUG_INFO ("CRC OK");
    
    return true;

    //return cipherFinished ();

}


void manageMessage (uint8_t* mac, uint8_t* buf, uint8_t count/*, void* cbarg*/) {
	//const uint8_t *buffer = buf;
    Node *node;

    DEBUG_INFO ("Reveived message. Origin MAC: %02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5] );
	DEBUG_VERBOSE ("Received data: %s", printHexBuffer ((byte *)buf, count));

    if (count <= 1) {
        DEBUG_WARN ("Empty message");
        return;
    }

    node = nodelist.getNewNode (mac);

    if (buf[0] != CLIENT_HELLO) {
		Crypto.decryptBuffer ((uint8_t *)buf, (uint8_t *)buf, count, node->getEncriptionKey());
		DEBUG_VERBOSE ("Decrypted data: %s", printHexBuffer ((byte *)buf, count));
    } 

	flashBlue = true;

    int espNowError;

	switch (buf[0]) {
	case CLIENT_HELLO:
        // TODO: Do no accept new Client Hello if registration is on process on any node
        // TODO: Check message length
        DEBUG_INFO (" <------- CLIENT HELLO");
        //if (WifiEspNow.addPeer (mac)) {
        espNowError = esp_now_add_peer (mac, ESP_NOW_ROLE_CONTROLLER, 0, NULL, 0);
        if (espNowError == 0) {
                if (processClientHello (mac, buf, count, node)) {
                node->setStatus (WAIT_FOR_KEY_EXCH_FINISHED);
                delay (1000);
                if (serverHello (myPublicKey, node)) {
                    DEBUG_INFO ("Server Hello sent");
                } else {
                    DEBUG_INFO ("Error sending Server Hello");
                }

            } else {
                node->reset ();
                DEBUG_ERROR ("Error processing client hello");
            }
        } else {
            DEBUG_ERROR ("Error adding peer %d", espNowError);
        }
        esp_now_del_peer (mac);
        //WifiEspNow.removePeer (mac);
        break;
    case KEY_EXCHANGE_FINISHED:
        // TODO: Check message length
        // TODO: Check that ongoing registration belongs to this mac address
        DEBUG_INFO ("Key Exchange Finished received");
        if (WifiEspNow.addPeer (mac)) {
            if (processKeyExchangeFinished (mac, buf, 1 + RANDOM_LENGTH + CRC_LENGTH)) {
                if (cipherFinished (node)) {
                    node->setStatus (REGISTERED);
                    nodelist.printToSerial ();
                    //DEBUG_INFO ("%s", nodelist.toString ().c_str());
                }
            }
        }
        WifiEspNow.removePeer (mac);
        break;

	}
}

void initEspNow () {
	/*bool ok = WifiEspNow.begin ();
	if (!ok) {
		Serial.println ("WifiEspNow.begin() failed");
		ESP.restart ();
	}
	WifiEspNow.onReceive (manageMessage, NULL);*/
    if (esp_now_init () != 0) {
        Serial.println ("Protocolo ESP-NOW no inicializado...");
        ESP.restart ();
        delay (1);
    }
    esp_now_set_self_role (ESP_NOW_ROLE_SLAVE);
    esp_now_register_recv_cb (manageMessage);
}

void setup () {
	//***INICIALIZACIÓN DEL PUERTO SERIE***//
	Serial.begin (115200); Serial.println (); Serial.println ();

	pinMode (BLUE_LED, OUTPUT);
	digitalWrite (BLUE_LED, HIGH);
	pinMode (RED_LED, OUTPUT);
	digitalWrite (RED_LED, HIGH);

	initWiFi ();
	initEspNow ();

}

void loop () {
#define LED_PERIOD 100
	static unsigned long blueOntime;
	static unsigned long redOntime;

	if (flashBlue) {
		blueOntime = millis ();
		digitalWrite (BLUE_LED, LOW);
		flashBlue = false;
	}

	if (!digitalRead(BLUE_LED) && millis () - blueOntime > LED_PERIOD) {
		digitalWrite (BLUE_LED, HIGH);
	}

	if (flashRed) {
		redOntime = millis ();
		digitalWrite (RED_LED, LOW);
		flashRed = false;
	}

	if (!digitalRead (RED_LED) && millis () - redOntime > LED_PERIOD) {
		digitalWrite (RED_LED, HIGH);
	}
}