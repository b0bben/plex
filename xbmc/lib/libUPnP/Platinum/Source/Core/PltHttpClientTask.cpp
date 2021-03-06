/*****************************************************************
|
|   Platinum - HTTP Client Tasks
|
| Copyright (c) 2004-2008, Plutinosoft, LLC.
| All rights reserved.
| http://www.plutinosoft.com
|
| This program is free software; you can redistribute it and/or
| modify it under the terms of the GNU General Public License
| as published by the Free Software Foundation; either version 2
| of the License, or (at your option) any later version.
|
| OEMs, ISVs, VARs and other distributors that combine and 
| distribute commercially licensed software with Platinum software
| and do not wish to distribute the source code for the commercially
| licensed software under version 2, or (at your option) any later
| version, of the GNU General Public License (the "GPL") must enter
| into a commercial license agreement with Plutinosoft, LLC.
| 
| This program is distributed in the hope that it will be useful,
| but WITHOUT ANY WARRANTY; without even the implied warranty of
| MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
| GNU General Public License for more details.
|
| You should have received a copy of the GNU General Public License
| along with this program; see the file LICENSE.txt. If not, write to
| the Free Software Foundation, Inc., 
| 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
| http://www.gnu.org/licenses/gpl-2.0.html
|
****************************************************************/

/*----------------------------------------------------------------------
|   includes
+---------------------------------------------------------------------*/
#include "PltHttpClientTask.h"

NPT_SET_LOCAL_LOGGER("platinum.core.http.clienttask")

/*----------------------------------------------------------------------
|   PLT_HttpTcpConnector::Connect
+---------------------------------------------------------------------*/
NPT_Result
PLT_HttpTcpConnector::Connect(const char*                hostname, 
                              NPT_UInt16                 port, 
                              NPT_Timeout                connection_timeout,
                              NPT_Timeout                io_timeout,
                              NPT_Timeout                name_resolver_timeout,
                              NPT_InputStreamReference&  input_stream, 
                              NPT_OutputStreamReference& output_stream)
{
    if (m_HostName == hostname && m_Port == port) {
        input_stream  = m_InputStream;
        output_stream = m_OutputStream;
        return NPT_SUCCESS;
    }

    // get the address and port to which we need to connect
    NPT_IpAddress address;
    NPT_CHECK_FATAL(address.ResolveName(hostname, name_resolver_timeout));

    // connect to the server
    NPT_LOG_FINE_2("NPT_HttpTcpConnector::Connect - will connect to %s:%d\n", hostname, port);
    m_Socket = new NPT_TcpClientSocket();
    m_Socket->SetReadTimeout(io_timeout);
    m_Socket->SetWriteTimeout(io_timeout);
    NPT_SocketAddress socket_address(address, port);
    NPT_CHECK_FATAL(m_Socket->Connect(socket_address, connection_timeout));

    // get and keep the streams
    NPT_CHECK(m_Socket->GetInputStream(m_InputStream));
    NPT_CHECK(m_Socket->GetOutputStream(m_OutputStream));
    NPT_CHECK(m_Socket->GetInfo(m_SocketInfo));

    m_HostName    = hostname;
    m_Port        = port;
    input_stream  = m_InputStream;
    output_stream = m_OutputStream;
    return NPT_SUCCESS;
}

/*----------------------------------------------------------------------
|   PLT_HttpClientSocketTask::PLT_HttpClientSocketTask
+---------------------------------------------------------------------*/
PLT_HttpClientSocketTask::PLT_HttpClientSocketTask(NPT_HttpRequest* request,
                                                   bool             wait_forever /* = false */) :
    m_WaitForever(wait_forever),
    m_Connector(NULL)
{
    if (request) m_Requests.Push(request);
}

/*----------------------------------------------------------------------
|   PLT_HttpClientSocketTask::~PLT_HttpClientSocketTask
+---------------------------------------------------------------------*/
PLT_HttpClientSocketTask::~PLT_HttpClientSocketTask()
{
    // delete any outstanding requests
    NPT_HttpRequest* request;
    while (NPT_SUCCEEDED(m_Requests.Pop(request, false))) {
        delete request;
    }
}

/*----------------------------------------------------------------------
|   PLT_HttpServerSocketTask::AddRequest
+---------------------------------------------------------------------*/
NPT_Result
PLT_HttpClientSocketTask::AddRequest(NPT_HttpRequest* request)
{
    return m_Requests.Push(request);
}

/*----------------------------------------------------------------------
|   PLT_HttpServerSocketTask::GetNextRequest
+---------------------------------------------------------------------*/
NPT_Result
PLT_HttpClientSocketTask::GetNextRequest(NPT_HttpRequest*& request, NPT_Timeout timeout)
{
    return m_Requests.Pop(request, timeout);
}

/*----------------------------------------------------------------------
|   PLT_HttpServerSocketTask::DoRun
+---------------------------------------------------------------------*/
void
PLT_HttpClientSocketTask::DoRun()
{
    NPT_HttpRequest*       request;
    NPT_HttpRequestContext context;
    bool                   using_previous_connector;
    NPT_Result             res;

    using_previous_connector = false;
    m_Client.SetConnector(m_Connector = new PLT_HttpTcpConnector());

    do {
        // pop next request or wait for one for 100ms
        if (NPT_SUCCEEDED(GetNextRequest(request, 100))) {
            NPT_HttpResponse* response = NULL;
retry:
            if (IsAborting(0)) {
                delete request;
                return;
            }

            // if body is not seekable, don't even try to
            // reuse previous connector since in case it fails because
            // server closed connection, we won't be able to
            // rewind the body to resend the request
            if (!PLT_HttpHelper::IsBodyStreamSeekable(*request) && using_previous_connector) {
                using_previous_connector = false;
                m_Client.SetConnector(m_Connector = new PLT_HttpTcpConnector());
            }

            // send request
            res = m_Client.SendRequest(*request, response);

            // retry only if we were reusing a previous connector
            if (NPT_FAILED(res) && using_previous_connector) {
                using_previous_connector = false;
                m_Client.SetConnector(m_Connector = new PLT_HttpTcpConnector());

                // server may have closed socket on us
                NPT_HttpEntity* entity = request->GetEntity();
                NPT_InputStreamReference input_stream;

                // rewind request body if any to be able to resend it
                if (entity && NPT_SUCCEEDED(entity->GetInputStream(input_stream))) {
                    input_stream->Seek(0);
                }

                goto retry;
            }

            NPT_LOG_FINE_1("PLT_HttpClientSocketTask receiving: res = %d", res);
            PLT_LOG_HTTP_MESSAGE(NPT_LOG_LEVEL_FINE, response);

            // callback to process response
            NPT_SocketInfo info;
            m_Connector->GetInfo(info);
            context.SetLocalAddress(info.local_address);
            context.SetRemoteAddress(info.remote_address);
            ProcessResponse(res, request, context, response);

            // check if server says keep-alive to keep our connector
            if (response && PLT_HttpHelper::IsConnectionKeepAlive(*response)) {
                using_previous_connector = true;
            } else {
                using_previous_connector = false;
                m_Client.SetConnector(m_Connector = new PLT_HttpTcpConnector());
            }

            // cleanup
            delete response;
            delete request;
        }
    } while (m_WaitForever && !IsAborting(0));
}

/*----------------------------------------------------------------------
|   PLT_HttpServerSocketTask::ProcessResponse
+---------------------------------------------------------------------*/
NPT_Result 
PLT_HttpClientSocketTask::ProcessResponse(NPT_Result                    res, 
                                          NPT_HttpRequest*              request, 
                                          const NPT_HttpRequestContext& context, 
                                          NPT_HttpResponse*             response) 
{
    NPT_COMPILER_UNUSED(request);
    NPT_COMPILER_UNUSED(context);

    NPT_LOG_FINE_1("PLT_HttpClientSocketTask::ProcessResponse (result=%d)", res);
    NPT_CHECK_SEVERE(res);

    NPT_HttpEntity* entity;
    NPT_InputStreamReference body;
    if (!response || 
        !(entity = response->GetEntity()) || 
        NPT_FAILED(entity->GetInputStream(body))) {
        return NPT_FAILURE;
    }

    // dump body into memory (if no content-length specified, read until disconnection)
    NPT_MemoryStream output;
    NPT_CHECK_SEVERE(NPT_StreamToStreamCopy(*body, 
                                            output,
                                            0, 
                                            entity->GetContentLength()));

    return NPT_SUCCESS;
}

/*----------------------------------------------------------------------
|   PLT_FileHttpClientTask::ProcessResponse
+---------------------------------------------------------------------*/
NPT_Result
PLT_FileHttpClientTask::ProcessResponse(NPT_Result                    res, 
                                        NPT_HttpRequest*              request, 
                                        const NPT_HttpRequestContext& context, 
                                        NPT_HttpResponse*             response) 
{
    NPT_COMPILER_UNUSED(res);
    NPT_COMPILER_UNUSED(request);
    NPT_COMPILER_UNUSED(context);
    NPT_COMPILER_UNUSED(response);

    NPT_LOG_FINE_1("PLT_FileHttpClientTask::ProcessResponse (status=%d)\n", res);
    return NPT_SUCCESS;
}
