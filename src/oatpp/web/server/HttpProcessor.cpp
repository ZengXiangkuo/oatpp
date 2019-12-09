/***************************************************************************
 *
 * Project         _____    __   ____   _      _
 *                (  _  )  /__\ (_  _)_| |_  _| |_
 *                 )(_)(  /(__)\  )( (_   _)(_   _)
 *                (_____)(__)(__)(__)  |_|    |_|
 *
 *
 * Copyright 2018-present, Leonid Stryzhevskyi <lganzzzo@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ***************************************************************************/

#include "HttpProcessor.hpp"

namespace oatpp { namespace web { namespace server {

HttpProcessor::Task::Task(const std::shared_ptr<HttpRouter>& router,
                          const std::shared_ptr<oatpp::data::stream::IOStream>& connection,
                          const std::shared_ptr<const oatpp::web::protocol::http::incoming::BodyDecoder>& bodyDecoder,
                          const std::shared_ptr<handler::ErrorHandler>& errorHandler,
                          const std::shared_ptr<HttpProcessor::RequestInterceptors>& requestInterceptors)
  : m_router(router)
  , m_connection(connection)
  , m_bodyDecoder(bodyDecoder)
  , m_errorHandler(errorHandler)
  , m_requestInterceptors(requestInterceptors)
{}

void HttpProcessor::Task::run(){

  m_connection->initContexts();

  const v_int32 bufferSize = oatpp::data::buffer::IOBuffer::BUFFER_SIZE;
  v_char8 bufferMemory[bufferSize];

  oatpp::data::share::MemoryLabel inBuffer(nullptr, bufferMemory, bufferSize);

  auto inStream = oatpp::data::stream::InputStreamBufferedProxy::createShared(m_connection, inBuffer);

  v_int32 connectionState = oatpp::web::protocol::http::outgoing::CommunicationUtils::CONNECTION_STATE_CLOSE;
  std::shared_ptr<oatpp::web::protocol::http::outgoing::Response> response;

  oatpp::data::stream::BufferOutputStream headersInBuffer(2048 /* initial capacity */, 2048 /* grow bytes */);
  oatpp::data::stream::BufferOutputStream headersOutBuffer(2048 /* initial capacity */, 2048 /* grow bytes */);
  oatpp::web::protocol::http::incoming::RequestHeadersReader headersReader(&headersInBuffer, 2048 /* read chunk size */, 4096 /* max headers size */);

  do {

    response = HttpProcessor::processRequest(m_router.get(), headersReader, inStream, m_bodyDecoder, m_errorHandler, m_requestInterceptors.get(), connectionState);

    if(response) {
      response->send(m_connection.get(), &headersOutBuffer);
    } else {
      return;
    }

  } while(connectionState == oatpp::web::protocol::http::outgoing::CommunicationUtils::CONNECTION_STATE_KEEP_ALIVE);

  if(connectionState == oatpp::web::protocol::http::outgoing::CommunicationUtils::CONNECTION_STATE_UPGRADE) {
    auto handler = response->getConnectionUpgradeHandler();
    if(handler) {
      handler->handleConnection(m_connection, response->getConnectionUpgradeParameters());
    } else {
      OATPP_LOGW("[oatpp::web::server::HttpConnectionHandler::Task::run()]", "Warning. ConnectionUpgradeHandler not set!");
    }
  }

}

std::shared_ptr<protocol::http::outgoing::Response>
HttpProcessor::processRequest(HttpRouter* router,
                              RequestHeadersReader& headersReader,
                              const std::shared_ptr<oatpp::data::stream::InputStreamBufferedProxy>& inStream,
                              const std::shared_ptr<const oatpp::web::protocol::http::incoming::BodyDecoder>& bodyDecoder,
                              const std::shared_ptr<handler::ErrorHandler>& errorHandler,
                              RequestInterceptors* requestInterceptors,
                              v_int32& connectionState) {



  oatpp::web::protocol::http::HttpError::Info error;
  auto headersReadResult = headersReader.readHeaders(inStream.get(), error);
  
  if(error.status.code != 0) {
    connectionState = oatpp::web::protocol::http::outgoing::CommunicationUtils::CONNECTION_STATE_CLOSE;
    return errorHandler->handleError(error.status, "Invalid request headers");
  }
  
  if(error.ioStatus <= 0) {
    connectionState = oatpp::web::protocol::http::outgoing::CommunicationUtils::CONNECTION_STATE_CLOSE;
    return nullptr; // connection is in invalid state. should be dropped
  }
  
  auto route = router->getRoute(headersReadResult.startingLine.method, headersReadResult.startingLine.path);
  
  if(!route) {
    connectionState = oatpp::web::protocol::http::outgoing::CommunicationUtils::CONNECTION_STATE_CLOSE;
    return errorHandler->handleError(protocol::http::Status::CODE_404, "Current url has no mapping");
  }
  
  auto request = protocol::http::incoming::Request::createShared(headersReadResult.startingLine,
                                                                 route.matchMap,
                                                                 headersReadResult.headers,
                                                                 inStream,
                                                                 bodyDecoder);
  
  std::shared_ptr<protocol::http::outgoing::Response> response;
  try{
    auto currInterceptor = requestInterceptors->getFirstNode();
    while (currInterceptor != nullptr) {
      response = currInterceptor->getData()->intercept(request);
      if(response) {
        break;
      }
      currInterceptor = currInterceptor->getNext();
    }
    if(!response) {
      response = route.getEndpoint()->handle(request);
    }
  } catch (oatpp::web::protocol::http::HttpError& error) {
    return errorHandler->handleError(error.getInfo().status, error.getMessage(), error.getHeaders());
  } catch (std::exception& error) {
    return errorHandler->handleError(protocol::http::Status::CODE_500, error.what());
  } catch (...) {
    return errorHandler->handleError(protocol::http::Status::CODE_500, "Unknown error");
  }
  
  response->putHeaderIfNotExists(protocol::http::Header::SERVER, protocol::http::Header::Value::SERVER);
  
  connectionState = oatpp::web::protocol::http::outgoing::CommunicationUtils::considerConnectionState(request, response);
  return response;
  
}
  
// HttpProcessor::Coroutine
  
HttpProcessor::Coroutine::Action HttpProcessor::Coroutine::act() {
  return m_connection->initContextsAsync().next(yieldTo(&HttpProcessor::Coroutine::parseHeaders));
}

HttpProcessor::Coroutine::Action HttpProcessor::Coroutine::parseHeaders() {
  return m_headersReader.readHeadersAsync(m_inStream).callbackTo(&HttpProcessor::Coroutine::onHeadersParsed);
}

oatpp::async::Action HttpProcessor::Coroutine::onHeadersParsed(const RequestHeadersReader::Result& headersReadResult) {

  m_currentRoute = m_router->getRoute(headersReadResult.startingLine.method.toString(), headersReadResult.startingLine.path.toString());

  if(!m_currentRoute) {
    m_currentResponse = m_errorHandler->handleError(protocol::http::Status::CODE_404, "Current url has no mapping");
    return yieldTo(&HttpProcessor::Coroutine::onResponseFormed);
  }

  m_currentRequest = protocol::http::incoming::Request::createShared(headersReadResult.startingLine,
                                                                     m_currentRoute.matchMap,
                                                                     headersReadResult.headers,
                                                                     m_inStream,
                                                                     m_bodyDecoder);

  auto currInterceptor = m_requestInterceptors->getFirstNode();
  while (currInterceptor != nullptr) {
    m_currentResponse = currInterceptor->getData()->intercept(m_currentRequest);
    if(m_currentResponse) {
      return yieldTo(&HttpProcessor::Coroutine::onResponseFormed);
    }
    currInterceptor = currInterceptor->getNext();
  }

  return yieldTo(&HttpProcessor::Coroutine::onRequestFormed);

}

HttpProcessor::Coroutine::Action HttpProcessor::Coroutine::onRequestFormed() {
  return m_currentRoute.getEndpoint()->handleAsync(m_currentRequest).callbackTo(&HttpProcessor::Coroutine::onResponse);
}

HttpProcessor::Coroutine::Action HttpProcessor::Coroutine::onResponse(const std::shared_ptr<protocol::http::outgoing::Response>& response) {
  m_currentResponse = response;
  return yieldTo(&HttpProcessor::Coroutine::onResponseFormed);
}
  
HttpProcessor::Coroutine::Action HttpProcessor::Coroutine::onResponseFormed() {

  m_currentResponse->putHeaderIfNotExists(protocol::http::Header::SERVER, protocol::http::Header::Value::SERVER);
  m_connectionState = oatpp::web::protocol::http::outgoing::CommunicationUtils::considerConnectionState(m_currentRequest, m_currentResponse);
  return protocol::http::outgoing::Response::sendAsync(m_currentResponse, m_connection, m_headersOutBuffer)
         .next(yieldTo(&HttpProcessor::Coroutine::onRequestDone));

}
  
HttpProcessor::Coroutine::Action HttpProcessor::Coroutine::onRequestDone() {
  
  if(m_connectionState == oatpp::web::protocol::http::outgoing::CommunicationUtils::CONNECTION_STATE_KEEP_ALIVE) {
    return yieldTo(&HttpProcessor::Coroutine::parseHeaders);
  }
  
  if(m_connectionState == oatpp::web::protocol::http::outgoing::CommunicationUtils::CONNECTION_STATE_UPGRADE) {
    auto handler = m_currentResponse->getConnectionUpgradeHandler();
    if(handler) {
      handler->handleConnection(m_connection, m_currentResponse->getConnectionUpgradeParameters());
    } else {
      OATPP_LOGD("[oatpp::web::server::HttpProcessor::Coroutine::onRequestDone()]", "Warning. ConnectionUpgradeHandler not set!");
    }
  }
  
  return finish();
}
  
HttpProcessor::Coroutine::Action HttpProcessor::Coroutine::handleError(Error* error) {

  if(error) {

    if(error->is<oatpp::data::AsyncIOError>()) {
      auto aioe = static_cast<oatpp::data::AsyncIOError*>(error);
      if(aioe->getCode() == oatpp::data::IOError::BROKEN_PIPE) {
        return aioe; // do not report BROKEN_PIPE error
      }
    }

    if(m_currentResponse) {
      OATPP_LOGE("[oatpp::web::server::HttpProcessor::Coroutine::handleError()]", "Unhandled error. '%s'. Dropping connection", error->what());
      return error;
    }

    m_currentResponse = m_errorHandler->handleError(protocol::http::Status::CODE_500, error->what());
    return yieldTo(&HttpProcessor::Coroutine::onResponseFormed);

  }

  return error;

}
  
}}}
