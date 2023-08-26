#include "JPEGDEC.h"
#include <TFT_eSPI.h>
#include <SPI.h>
#include "secrets.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <MQTTClient.h>
#include <ArduinoJson.h>
#include "WiFi.h"

TFT_eSPI tft = TFT_eSPI();

// TFT_eSPI tft = TFT_eSPI(240, 320);       // Could invoke custom library declaring width and height

unsigned long targetTime = 0;
byte red = 31;
byte green = 0;
byte blue = 0;
byte state = 0;
unsigned int colour = red << 11; // Colour order is RGB 5+6+5 bits each

// 50kb
const uint16_t FRAME_BUFFER_SIZE = 50000;
uint8_t *video_frame_buffer;

// The MQTT topics that this device should publish/subscribe
#define AWS_IOT_PUBLISH_TOPIC "esp32_photo_frame/metrics"
#define AWS_IOT_SUBSCRIBE_TOPIC_1 "new_image_available"
#define AWS_IOT_SUBSCRIBE_TOPIC_2 "command/device/esp32_photo_frame"

WiFiClientSecure net = WiFiClientSecure();
WiFiClientSecure net2 = WiFiClientSecure();
HTTPClient https;
MQTTClient client = MQTTClient(512);
JPEGDEC jpeg;

void connect_wifi()
{
  WiFi.disconnect();
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  Serial.println("Connecting to Wi-Fi");
  uint8_t counter = 0;
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(1000);
    Serial.print(".");
    if (counter > 30)
    {
      esp_restart();
    }
    counter += 1;
  }
}

void connectAWS()
{
  // Configure WiFiClientSecure to use the AWS IoT device credentials
  net.setCACert(AWS_CERT_CA);
  net.setCertificate(AWS_CERT_CRT);
  net.setPrivateKey(AWS_CERT_PRIVATE);

  // Connect to the MQTT broker on the AWS endpoint we defined earlier
  client.begin(AWS_IOT_ENDPOINT, 8883, net);

  // Create a message handler
  client.onMessage(messageHandler);

  Serial.print("Connecting to AWS IOT");
  uint8_t counter = 0;
  if (!client.connect(THINGNAME))
  {
    Serial.print(".");
    Serial.printf("clientMQTT.lastError = %d\n", client.lastError());
    Serial.printf("clientMQTT.returnCode = %d\n", client.returnCode());
    delay(5000);
    esp_restart();
  }

  // Subscribe to a topic
  if (client.subscribe(AWS_IOT_SUBSCRIBE_TOPIC_1))
  {
    Serial.println("Successfully subscribed to Topic 1");
  }
  if (client.subscribe(AWS_IOT_SUBSCRIBE_TOPIC_2))
  {
    Serial.println("Successfully subscribed to Topic 2");
  }

  Serial.println("AWS IoT Connected!");
  get_image();
}

void publish_metrics()
{
  StaticJsonDocument<200> doc;
  doc["signal_strength"] = WiFi.RSSI();
  doc["ip_address"] = WiFi.localIP().toString();
  doc["temperature"] = temperatureRead();
  doc["heap_memory_free_bytes"] = esp_get_free_heap_size();
  char jsonBuffer[512];
  serializeJson(doc, jsonBuffer); // print to client
  client.publish(AWS_IOT_PUBLISH_TOPIC, jsonBuffer);
}

void messageHandler(String &topic, String &payload)
{
  // Serial.println("incoming: " + topic + " - " + payload);
  if (topic == AWS_IOT_SUBSCRIBE_TOPIC_1)
  {
    get_image();
  }
  else if (topic == AWS_IOT_SUBSCRIBE_TOPIC_2)
  {
    if (payload == "OFF")
    {
      // Serial.println("Turning off display");
      tft.writecommand(0x10);
    }
    if (payload == "ON")
    {
      // Serial.println("Turning on display");
      tft.init();
    }
    if (payload == "RESTART")
    {
      // Serial.println("Restarting system");
      esp_restart();
    }
    if (payload == "METRICS")
    {
      publish_metrics();
    }
  }
}

void setup(void)
{
  Serial.begin(115200);
  tft.init();
  tft.setRotation(2);
  tft.fillScreen(TFT_BLACK);
  video_frame_buffer = (uint8_t *)malloc(sizeof(uint8_t) * FRAME_BUFFER_SIZE);
  connect_wifi();
  connectAWS();
}

int JPEGDraw(JPEGDRAW *pDraw)
{
  int iCount;
  iCount = pDraw->iWidth * pDraw->iHeight; // number of pixels to draw in this call
  // Serial.printf("Drawing %d pixels\n", iCount);
  tft.pushImage(pDraw->x, pDraw->y, pDraw->iWidth, pDraw->iHeight, pDraw->pPixels);
  return 1;
}

void get_image()
{
  // Serial.println("Grabbing image");
  net2.setCACert(AWS_CERT_CA);
  https.begin(net2, HOST_URL);
  https.addHeader("Accept", "image/jpeg");
  https.addHeader("x-api-token", API_SECRET);
  uint16_t timeout = 8000;
  https.setTimeout(timeout);
  int response = https.GET();
  // Serial.println(response);
  if (response > 0)
  {
    if (response == HTTP_CODE_OK)
    {
      int original_length = https.getSize();
      if (original_length < FRAME_BUFFER_SIZE)
      {
        int len = original_length;
        WiFiClient *stream = https.getStreamPtr();
        // read all data from server
        while (https.connected() && len > 0)
        {
          // get available data size
          size_t size = stream->available();
          // Serial.println(size);
          if (size)
          {
            int c = stream->readBytes(video_frame_buffer, len);
            if (len > 0)
            {
              len -= c;
            }
          }
          delay(1);
        }
        // display image
        tft.startWrite();
        if (jpeg.openRAM(video_frame_buffer, original_length, JPEGDraw))
        {
          jpeg.setPixelType(RGB565_BIG_ENDIAN);
          if (jpeg.decode(0, 0, 0))
          {
            tft.endWrite();
          }
          else
          {
            Serial.println("Decode error");
          }
          jpeg.close();
        }
        else
        {
          Serial.println("Failed to open JPEG");
        }
      }
    }
    else
    {
      Serial.println("response failed");
    }
    // Serial.println("Done");
  }
}

void loop()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    if (client.connected())
    {
      client.loop();
    }
    else
    {
      connectAWS();
    }
  }
  else
  {
    connect_wifi();
  }
}
