/* 
 *  Most of the code here is provided by me-no-dev
 *  This guy is a legend
 *  https://gist.github.com/me-no-dev/d34fba51a8f059ac559bf62002e61aa3
 */
 
typedef struct {
  camera_fb_t * fb;
  size_t index;
} camera_frame_t;

#define PART_BOUNDARY "123456789000000000000987654321"
#define JPG_CONTENT_TYPE "image/jpeg"

static const char* STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* STREAM_PART = "Content-Type: %s\r\nContent-Length: %u\r\n\r\n";

class AsyncFrameResponse: public AsyncAbstractResponse {
 
  private:
    camera_fb_t * fb;
    size_t _index;
        
  public:
    AsyncFrameResponse(camera_fb_t * frame, const char * contentType){
      _callback = nullptr;
      _code = 200;
      _contentLength = frame->len;
      _contentType = contentType;
      _index = 0;
      fb = frame;
    }
      
    ~AsyncFrameResponse(){
      // Destructor: clears frame buffer
      if(fb != nullptr) esp_camera_fb_return(fb);
    }
    
    bool _sourceValid() const { 
      return fb != nullptr;
    }
      
    virtual size_t _fillBuffer(uint8_t *buf, size_t maxLen) override{
      size_t ret = _content(buf, maxLen, _index);
      if(ret != RESPONSE_TRY_AGAIN) _index += ret;
      return ret;
    }
      
    size_t _content(uint8_t *buffer, size_t maxLen, size_t index){
      memcpy(buffer, fb->buf+index, maxLen);

      // clear frame buffer
      if((index+maxLen) == fb->len){
        esp_camera_fb_return(fb);
        fb = nullptr;
      }
      
      return maxLen;
    }
};

class AsyncJpegStreamResponse: public AsyncAbstractResponse {
  private:
    camera_frame_t _frame;
    size_t _index;
      
  public:
    AsyncJpegStreamResponse(){
      // constructor
      _callback = nullptr;
      _code = 200;
      _contentLength = 0;
      _contentType = STREAM_CONTENT_TYPE;
      _sendContentLength = false;
      _chunked = true;
      _index = 0;
      memset(&_frame, 0, sizeof(camera_frame_t));
    }
      
    ~AsyncJpegStreamResponse(){
      // Destructor: Clear frame buffer
      if(_frame.fb) esp_camera_fb_return(_frame.fb);
    }
    bool _sourceValid() const {
      // Seems a bit pointless but provably overrides parent class
      return true;
    }
      
    virtual size_t _fillBuffer(uint8_t *buf, size_t maxLen) override {
      size_t ret = _content(buf, maxLen, _index);
      if(ret != RESPONSE_TRY_AGAIN)_index += ret;
      return ret;
    }
      
    size_t _content(uint8_t *buffer, size_t maxLen, size_t index){

      if(!_frame.fb || _frame.index == _frame.fb->len){

          if(index && _frame.fb){
              // clear frame buffer
              esp_camera_fb_return(_frame.fb);
              _frame.fb = NULL;
          }



          // Check for header lengths
          if(maxLen < (strlen(STREAM_BOUNDARY) + strlen(STREAM_PART) + strlen(JPG_CONTENT_TYPE) + 8)){
            log_w("Not enough space for headers");
            return RESPONSE_TRY_AGAIN;
          }
          
          // get frame
          _frame.index = 0;
          _frame.fb = esp_camera_fb_get();

          // Check if frame was taken successfully
          if (_frame.fb == NULL) {
            log_e("Camera frame failed");
            return 0;
          }


          //send boundary
          size_t boundary_length = 0;
          if(index){
            boundary_length = strlen(STREAM_BOUNDARY);
            memcpy(buffer, STREAM_BOUNDARY, boundary_length);
            buffer += boundary_length;
          }
          
          //send header
          size_t headers_length = sprintf((char *)buffer, STREAM_PART, JPG_CONTENT_TYPE, _frame.fb->len);
          buffer += headers_length;
          
          //send frame
          headers_length = maxLen - headers_length - boundary_length;
          if(headers_length > _frame.fb->len){
            maxLen -= headers_length - _frame.fb->len;
            headers_length = _frame.fb->len;
          }
          memcpy(buffer, _frame.fb->buf, headers_length);
          _frame.index += headers_length;
          return maxLen;
      }

      size_t available = _frame.fb->len - _frame.index;
      if(maxLen > available){
        maxLen = available;
      }
      memcpy(buffer, _frame.fb->buf+_frame.index, maxLen);
      _frame.index += maxLen;

      return maxLen;
    }
};


void notFound(AsyncWebServerRequest *request) {
  request->send(404, "text/plain", "Not found");
}

void sendJpg(AsyncWebServerRequest *request){
  Serial.println("[HTTP] /frame requested");
  
  camera_fb_t * fb = esp_camera_fb_get();
  if (fb == NULL) {
    log_e("Camera frame failed");
    request->send(501);
    return;
  }

  // Note: Images are in JPEG because camera configured as such in camera.ino
  AsyncFrameResponse * response = new AsyncFrameResponse(fb, JPG_CONTENT_TYPE);
  if (response == NULL) {
    log_e("Response alloc failed");
    request->send(501);
    return;
  }
  response->addHeader("Access-Control-Allow-Origin", "*");
  request->send(response);
  return;

}


void streamJpg(AsyncWebServerRequest *request){
  Serial.println("[HTTP] /stream requested");
  AsyncJpegStreamResponse *response = new AsyncJpegStreamResponse();
  
  if(!response){
    request->send(501);
    return;
  }
  
  response->addHeader("Access-Control-Allow-Origin", "*");
  request->send(response);
}

void handleDoUpdate(AsyncWebServerRequest *request, const String& filename, size_t index, uint8_t *data, size_t len, bool final) {
  
  if (!index){
    Serial.println("[Web server] Firmware update started");
    //size_t content_len = request->contentLength();
    // if filename includes spiffs, update the spiffs partition
    int cmd = (filename.indexOf("spiffs") > -1) ? U_PART : U_FLASH;


    if (!Update.begin(UPDATE_SIZE_UNKNOWN, cmd)) {
      Update.printError(Serial);
    }
  }

  if (Update.write(data, len) != len) {
    Update.printError(Serial);
  }

  if (final) {
    AsyncWebServerResponse *response = request->beginResponse(302, "text/plain", "Please wait while the device reboots");
    response->addHeader("Refresh", "20");  
    response->addHeader("Location", "/");
    request->send(response);
    if (!Update.end(true)){
      Update.printError(Serial);
    } else {
      Serial.println("[Web server] Firmware update complete");
      Serial.flush();
      ESP.restart();
    }
  }
}

void handle_not_found(AsyncWebServerRequest *request) {
  request->send(404, "text/html", "Not found");
}

void handle_update_form(AsyncWebServerRequest *request){
  String html = apply_html_template(firmware_update_form);
  request->send(200, "text/html", html);
}

void handle_homepage(AsyncWebServerRequest *request) {
  String html = apply_html_template(get_homepage());
  request->send(200, "text/html", html);
}


void server_init(){

  Serial.println("[HTTP] Web server init");
  
  server.on("/", HTTP_GET, handle_homepage);

  server.on("/frame", HTTP_GET, sendJpg);
  server.on("/stream", HTTP_GET, streamJpg);

  server.on("/update", HTTP_GET, handle_update_form);
  server.on("/update", HTTP_POST,
    [](AsyncWebServerRequest *request) {},
    [](AsyncWebServerRequest *request, const String& filename, size_t index, uint8_t *data,
                  size_t len, bool final) {handleDoUpdate(request, filename, index, data, len, final);}
  );


  server.onNotFound(notFound);

  server.begin();
}
