//
//  Copyright (c) 2014-2016 plan44.ch / Lukas Zeller, Zurich, Switzerland
//
//  Author: Lukas Zeller <luz@plan44.ch>
//
//  This file is part of vdcd.
//
//  vdcd is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  vdcd is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with vdcd. If not, see <http://www.gnu.org/licenses/>.
//

#include "p44_vdcd_host.hpp"

#include "vdc.hpp"
#include "device.hpp"

#include "jsonvdcapi.hpp"

using namespace p44;


// MARK: ===== config API - P44JsonApiRequest


P44JsonApiRequest::P44JsonApiRequest(JsonCommPtr aJsonComm)
{
  jsonComm = aJsonComm;
}



ErrorPtr P44JsonApiRequest::sendResult(ApiValuePtr aResult)
{
  LOG(LOG_DEBUG, "cfg <- vdcd (JSON) result sent: result=%s", aResult ? aResult->description().c_str() : "<none>");
  JsonApiValuePtr result = boost::dynamic_pointer_cast<JsonApiValue>(aResult);
  if (result) {
    P44VdcHost::sendCfgApiResponse(jsonComm, result->jsonObject(), ErrorPtr());
  }
  else {
    // always return SOMETHING
    P44VdcHost::sendCfgApiResponse(jsonComm, JsonObject::newNull(), ErrorPtr());
  }
  return ErrorPtr();
}



ErrorPtr P44JsonApiRequest::sendError(uint32_t aErrorCode, string aErrorMessage, ApiValuePtr aErrorData)
{
  LOG(LOG_DEBUG, "cfg <- vdcd (JSON) error sent: error=%d (%s)", aErrorCode, aErrorMessage.c_str());
  ErrorPtr err = ErrorPtr(new Error(aErrorCode, aErrorMessage));
  P44VdcHost::sendCfgApiResponse(jsonComm, JsonObjectPtr(), err);
  return ErrorPtr();
}


ApiValuePtr P44JsonApiRequest::newApiValue()
{
  return ApiValuePtr(new JsonApiValue);
}


// MARK: ===== perform self test


class SelfTestRunner
{
  StatusCB completedCB;
  VdcMap::iterator nextVdc;
  VdcHost &vdcHost;
  ButtonInputPtr button;
  IndicatorOutputPtr redLED;
  IndicatorOutputPtr greenLED;
  long errorReportTicket;
  ErrorPtr globalError;
public:
  static void initialize(VdcHost &aVdcHost, StatusCB aCompletedCB, ButtonInputPtr aButton, IndicatorOutputPtr aRedLED, IndicatorOutputPtr aGreenLED)
  {
    // create new instance, deletes itself when finished
    new SelfTestRunner(aVdcHost, aCompletedCB, aButton, aRedLED, aGreenLED);
  };
private:
  SelfTestRunner(VdcHost &aVdcHost, StatusCB aCompletedCB, ButtonInputPtr aButton, IndicatorOutputPtr aRedLED, IndicatorOutputPtr aGreenLED) :
  completedCB(aCompletedCB),
  vdcHost(aVdcHost),
  button(aButton),
  redLED(aRedLED),
  greenLED(aGreenLED),
  errorReportTicket(0)
  {
    // start testing
    nextVdc = vdcHost.vdcs.begin();
    testNextContainer();
  }


  void testNextContainer()
  {
    if (nextVdc!=vdcHost.vdcs.end()) {
      // ok, test next
      // - start green/yellow blinking = test in progress
      greenLED->steadyOn();
      redLED->blinkFor(Infinite, 600*MilliSecond, 50);
      // - run the test
      LOG(LOG_WARNING, "Starting Test of %s (Tag=%d, %s)", nextVdc->second->vdcClassIdentifier(), nextVdc->second->getTag(), nextVdc->second->shortDesc().c_str());
      nextVdc->second->selfTest(boost::bind(&SelfTestRunner::containerTested, this, _1));
    }
    else
      testCompleted(); // done
  }


  void containerTested(ErrorPtr aError)
  {
    if (!Error::isOK(aError)) {
      // test failed
      LOG(LOG_ERR, "****** Test of '%s' FAILED with error: %s", nextVdc->second->vdcClassIdentifier(), aError->description().c_str());
      // remember
      globalError = aError;
      // morse out tag number of vDC failing self test until button is pressed
      greenLED->steadyOff();
      int numBlinks = nextVdc->second->getTag();
      redLED->blinkFor(300*MilliSecond*numBlinks, 300*MilliSecond, 50);
      // call myself again later
      errorReportTicket = MainLoop::currentMainLoop().executeOnce(boost::bind(&SelfTestRunner::containerTested, this, aError), 300*MilliSecond*numBlinks+2*Second);
      // also install button responder
      button->setButtonHandler(boost::bind(&SelfTestRunner::errorAcknowledged, this), false); // report only release
    }
    else {
      // test was ok
      LOG(LOG_ERR, "------ Test of '%s' OK", nextVdc->second->vdcClassIdentifier());
      // check next
      ++nextVdc;
      testNextContainer();
    }
  }


  void errorAcknowledged()
  {
    // stop error morse
    redLED->steadyOff();
    greenLED->steadyOff();
    MainLoop::currentMainLoop().cancelExecutionTicket(errorReportTicket);
    // test next (if any)
    ++nextVdc;
    testNextContainer();
  }


  void testCompleted()
  {
    if (Error::isOK(globalError)) {
      LOG(LOG_ERR, "Self test OK");
      redLED->steadyOff();
      greenLED->blinkFor(Infinite, 500, 85); // slow green blinking = good
    }
    else  {
      LOG(LOG_ERR, "Self test has FAILED");
      greenLED->steadyOff();
      redLED->blinkFor(Infinite, 250, 60); // faster red blinking = not good
    }
    // callback, report last error seen
    completedCB(globalError);
    // done, delete myself
    delete this;
  }

};


void P44VdcHost::selfTest(StatusCB aCompletedCB, ButtonInputPtr aButton, IndicatorOutputPtr aRedLED, IndicatorOutputPtr aGreenLED)
{
  SelfTestRunner::initialize(*this, aCompletedCB, aButton, aRedLED, aGreenLED);
}



string P44VdcHost::webuiURLString()
{
  if (webUiPort)
    return string_format("http://%s:%d", ipv4AddressString().c_str(), webUiPort);
  else
    return ""; // none
}



// MARK: ===== Config API


P44VdcHost::P44VdcHost() :
  learnIdentifyTicket(0),
  webUiPort(0)
{
  configApiServer = SocketCommPtr(new SocketComm(MainLoop::currentMainLoop()));
}


void P44VdcHost::startConfigApi()
{
  configApiServer->startServer(boost::bind(&P44VdcHost::configApiConnectionHandler, this, _1), 3);
}



SocketCommPtr P44VdcHost::configApiConnectionHandler(SocketCommPtr aServerSocketCommP)
{
  JsonCommPtr conn = JsonCommPtr(new JsonComm(MainLoop::currentMainLoop()));
  conn->setMessageHandler(boost::bind(&P44VdcHost::configApiRequestHandler, this, conn, _1, _2));
  conn->setClearHandlersAtClose(); // close must break retain cycles so this object won't cause a mem leak
  return conn;
}


void P44VdcHost::configApiRequestHandler(JsonCommPtr aJsonComm, ErrorPtr aError, JsonObjectPtr aJsonObject)
{
  ErrorPtr err;
  // when coming from mg44, requests have the following form
  // - for GET requests like http://localhost:8080/api/json/myuri?foo=bar&this=that
  //   {"method":"GET","uri":"myuri","uri_params":{"foo":"bar","this":"that"}}
  // - for POST requests like
  //   curl "http://localhost:8080/api/json/myuri?foo=bar&this=that" --data-ascii "{ \"content\":\"data\", \"important\":false }"
  //   {"method":"POST","uri":"myuri","uri_params":{"foo":"bar","this":"that"},"data":{"content":"data","important":false}}
  //   curl "http://localhost:8080/api/json/myuri" --data-ascii "{ \"content\":\"data\", \"important\":false }"
  //   {"method":"POST","uri":"myuri","data":{"content":"data","important":false}}
  // processing:
  // - a JSON request must be either specified in the URL or in the POST data, not both
  // - if POST data ("data" member in the incoming request) is present, "uri_params" is ignored
  // - "uri" selects one of possibly multiple APIs
  if (Error::isOK(aError)) {
    // not JSON level error, try to process
    LOG(LOG_DEBUG, "cfg -> vdcd (JSON) request received: %s", aJsonObject->c_strValue());
    // find out which one is our actual JSON request
    // - try POST data first
    JsonObjectPtr request = aJsonObject->get("data");
    if (!request) {
      // no POST data, try uri_params
      request = aJsonObject->get("uri_params");
    }
    if (!request) {
      // empty query, that's an error
      aError = ErrorPtr(new P44VdcError(415, "empty request"));
    }
    else {
      // have the request processed
      string apiselector;
      JsonObjectPtr uri = aJsonObject->get("uri");
      if (uri) apiselector = uri->stringValue();
      // dispatch according to API
      if (apiselector=="vdc") {
        // process request that basically is a vdc API request, but as simple webbish JSON, not as JSON-RPC 2.0
        // and without the need to start a vdc session
        // Notes:
        // - if dSUID is specified invalid or empty, the vdc host itself is addressed.
        // - use x-p44-vdcs and x-p44-devices properties to find dsuids
        aError = processVdcRequest(aJsonComm, request);
      }
      else if (apiselector=="p44") {
        // process p44 specific requests
        aError = processP44Request(aJsonComm, request);
      }
      else {
        // unknown API selector
        aError = ErrorPtr(new P44VdcError(400, "invalid URI, unknown API"));
      }
    }
  }
  // if error or explicit OK, send response now. Otherwise, request processing will create and send the response
  if (aError) {
    sendCfgApiResponse(aJsonComm, JsonObjectPtr(), aError);
  }
}


void P44VdcHost::sendCfgApiResponse(JsonCommPtr aJsonComm, JsonObjectPtr aResult, ErrorPtr aError)
{
  // create response
  JsonObjectPtr response = JsonObject::newObj();
  if (!Error::isOK(aError)) {
    // error, return error response
    response->add("error", JsonObject::newInt32((int32_t)aError->getErrorCode()));
    response->add("errormessage", JsonObject::newString(aError->getErrorMessage()));
    response->add("errordomain", JsonObject::newString(aError->getErrorDomain()));
  }
  else {
    // no error, return result (if any)
    response->add("result", aResult);
  }
  LOG(LOG_DEBUG, "Config API response: %s", response->c_strValue());
  aJsonComm->sendMessage(response);
}


// access to vdc API methods and notifications via web requests
ErrorPtr P44VdcHost::processVdcRequest(JsonCommPtr aJsonComm, JsonObjectPtr aRequest)
{
  ErrorPtr err;
  string cmd;
  bool isMethod = false;
  // get method/notification and params
  JsonObjectPtr m = aRequest->get("method");
  if (m) {
    // is a method call, expects answer
    isMethod = true;
  }
  else {
    // not method, may be notification
    m = aRequest->get("notification");
  }
  if (!m) {
    err = ErrorPtr(new P44VdcError(400, "invalid request, must specify 'method' or 'notification'"));
  }
  else {
    // get method/notification name
    cmd = m->stringValue();
    // get params
    // Note: the "method" or "notification" param will also be in the params, but should not cause any problem
    ApiValuePtr params = JsonApiValue::newValueFromJson(aRequest);
    ApiValuePtr o;
    err = checkParam(params, "dSUID", o);
    if (Error::isOK(err)) {
      // operation method
      DsUid dsuid;
      if (isMethod) {
        dsuid.setAsBinary(o->binaryValue());
        // create request
        P44JsonApiRequestPtr request = P44JsonApiRequestPtr(new P44JsonApiRequest(aJsonComm));
        // check for old-style name/index and generate basic query (1 or 2 levels)
        ApiValuePtr query = params->newObject();
        ApiValuePtr name = params->get("name");
        if (name) {
          ApiValuePtr index = params->get("index");
          ApiValuePtr subquery = params->newNull();
          if (index) {
            // subquery
            subquery->setType(apivalue_object);
            subquery->add(index->stringValue(), subquery->newNull());
          }
          string nm = trimWhiteSpace(name->stringValue()); // to allow a single space for deep recursing wildcard
          query->add(nm, subquery);
          params->add("query", query);
        }
        // have method handled
        err = handleMethodForDsUid(cmd, request, dsuid, params);
        // methods send results themselves
        if (Error::isOK(err)) {
          err.reset(); // even if we get a ErrorOK, make sure we return NULL to the caller, meaning NO answer is needed
        }
      }
      else {
        // handle notification
        // dSUID param can be single dSUID or array of dSUIDs
        if (o->isType(apivalue_array)) {
          // array of dSUIDs
          for (int i=0; i<o->arrayLength(); i++) {
            ApiValuePtr e = o->arrayGet(i);
            dsuid.setAsBinary(e->binaryValue());
            handleNotificationForDsUid(cmd, dsuid, params);
          }
        }
        else {
          // single dSUID
          dsuid.setAsBinary(o->binaryValue());
          handleNotificationForDsUid(cmd, dsuid, params);
        }
        // notifications are always successful
        err = ErrorPtr(new Error(ErrorOK));
      }
    }
  }
  // returning NULL means caller should not do anything more
  // returning an Error object (even ErrorOK) means caller should return status
  return err;
}


// access to plan44 extras that are not part of the vdc API
ErrorPtr P44VdcHost::processP44Request(JsonCommPtr aJsonComm, JsonObjectPtr aRequest)
{
  ErrorPtr err;
  JsonObjectPtr m = aRequest->get("method");
  if (!m) {
    err = ErrorPtr(new P44VdcError(400, "missing 'method'"));
  }
  else {
    string method = m->stringValue();
    if (method=="learn") {
      // check proximity check disabling
      bool disableProximity = false;
      JsonObjectPtr o = aRequest->get("disableProximityCheck");
      if (o) {
        disableProximity = o->boolValue();
      }
      // get timeout
      o = aRequest->get("seconds");
      int seconds = 30; // default to 30
      if (o) seconds = o->int32Value();
      if (seconds==0) {
        // end learning prematurely
        stopLearning();
        MainLoop::currentMainLoop().cancelExecutionTicket(learnIdentifyTicket);
        // - close still running learn request
        if (learnIdentifyRequest) {
          learnIdentifyRequest->closeConnection();
          learnIdentifyRequest.reset();
        }
        // - confirm abort with no result
        sendCfgApiResponse(aJsonComm, JsonObjectPtr(), ErrorPtr());
      }
      else {
        // start learning
        learnIdentifyRequest = aJsonComm; // remember so we can cancel it when we receive a separate cancel request
        startLearning(boost::bind(&P44VdcHost::learnHandler, this, aJsonComm, _1, _2), disableProximity);
        learnIdentifyTicket = MainLoop::currentMainLoop().executeOnce(boost::bind(&P44VdcHost::learnHandler, this, aJsonComm, false, ErrorPtr(new P44VdcError(408, "learn timeout"))), seconds*Second);
      }
    }
    else if (method=="identify") {
      // get timeout
      JsonObjectPtr o = aRequest->get("seconds");
      int seconds = 30; // default to 30
      if (o) seconds = o->int32Value();
      if (seconds==0) {
        // end reporting user activity
        setUserActionMonitor(NULL);
        MainLoop::currentMainLoop().cancelExecutionTicket(learnIdentifyTicket);
        // - close still running identify request
        if (learnIdentifyRequest) {
          learnIdentifyRequest->closeConnection();
          learnIdentifyRequest.reset();
        }
        // - confirm abort with no result
        sendCfgApiResponse(aJsonComm, JsonObjectPtr(), ErrorPtr());
      }
      else {
        // wait for next user activity
        learnIdentifyRequest = aJsonComm; // remember so we can cancel it when we receive a separate cancel request
        setUserActionMonitor(boost::bind(&P44VdcHost::identifyHandler, this, aJsonComm, _1));
        learnIdentifyTicket = MainLoop::currentMainLoop().executeOnce(boost::bind(&P44VdcHost::identifyHandler, this, aJsonComm, DevicePtr()), seconds*Second);
      }
    }
    else if (method=="logLevel") {
      // get or set logging level for vdcd
      JsonObjectPtr o = aRequest->get("value");
      if (o) {
        // set new value first
        int newLevel = o->int32Value();
        int oldLevel = LOGLEVEL;
        SETLOGLEVEL(newLevel);
        LOG(LOG_WARNING, "\n\n========== changed log level from %d to %d ===============", oldLevel, newLevel);
      }
      // anyway: return current value
      sendCfgApiResponse(aJsonComm, JsonObject::newInt32(LOGLEVEL), ErrorPtr());
    }
    else {
      err = ErrorPtr(new P44VdcError(400, "unknown method"));
    }
  }
  return err;
}


void P44VdcHost::learnHandler(JsonCommPtr aJsonComm, bool aLearnIn, ErrorPtr aError)
{
  MainLoop::currentMainLoop().cancelExecutionTicket(learnIdentifyTicket);
  stopLearning();
  sendCfgApiResponse(aJsonComm, JsonObject::newBool(aLearnIn), aError);
  learnIdentifyRequest.reset();
}


void P44VdcHost::identifyHandler(JsonCommPtr aJsonComm, DevicePtr aDevice)
{
  MainLoop::currentMainLoop().cancelExecutionTicket(learnIdentifyTicket);
  if (aDevice) {
    sendCfgApiResponse(aJsonComm, JsonObject::newString(aDevice->getDsUid().getString()), ErrorPtr());
    // end monitor mode
    setUserActionMonitor(NULL);
  }
  else {
    sendCfgApiResponse(aJsonComm, JsonObjectPtr(), ErrorPtr(new P44VdcError(408, "identify timeout")));
    setUserActionMonitor(NULL);
  }
  learnIdentifyRequest.reset();
}


// MARK: ===== self test procedure



