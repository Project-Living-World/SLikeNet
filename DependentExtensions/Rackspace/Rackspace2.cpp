/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2016-2020, SLikeSoft UG (haftungsbeschränkt)
 *
 *  This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

#include "Rackspace2.h"
#include "slikenet/TCPInterface.h"
#include "slikenet/HTTPConnection2.h"
#include "slikenet/linux_adapter.h"
#include "slikenet/osx_adapter.h"

using namespace SLNet;

Rackspace2::Rackspace2()
{
	X_Auth_Token[0]=0;
	eventCallback=0;
	tcp=0;
	cloudAccountNumber=0;
	reexecuteLastRequestOnAuth=false;
}
Rackspace2::~Rackspace2()
{

}
void Rackspace2::Update(void)
{
	RakString stringTransmitted;
	RakString hostTransmitted;
	RakString responseReceived;
	Packet *packet;
	SystemAddress sa;
	// This is kind of crappy, but for TCP plugins, always do HasCompletedConnectionAttempt, then Receive(), then HasFailedConnectionAttempt(),HasLostConnection()
	sa = tcp->HasCompletedConnectionAttempt();
	if (sa!=UNASSIGNED_SYSTEM_ADDRESS)
	{
		//printf("Rackspace2 TCP: Connected to %s\n", sa.ToString());
//		serverAddress = sa;
	}
	for (packet = tcp->Receive(); packet; tcp->DeallocatePacket(packet), packet = tcp->Receive())
		;
	sa = tcp->HasFailedConnectionAttempt();
	//if (sa!=UNASSIGNED_SYSTEM_ADDRESS)
	//	printf("Rackspace2 TCP: Failed connection attempt to %s\n", sa.ToString());
	sa = tcp->HasLostConnection();
	if (sa!=UNASSIGNED_SYSTEM_ADDRESS)
	{
		//printf("Rackspace2 TCP: Lost connection to %s\n", sa.ToString());
		//		serverAddress=UNASSIGNED_SYSTEM_ADDRESS;
	}

	SystemAddress hostReceived;
	ptrdiff_t contentOffset;
	if (httpConnection2->GetResponse(stringTransmitted, hostTransmitted, responseReceived, hostReceived, contentOffset))
	{
		if (responseReceived.IsEmpty()==false)
		{
			static FILE *fp = nullptr;
			if (fp == nullptr)
				fopen_s(&fp, "responses.txt", "wt");
			// #low - add nullptr-check (incl. error logging) for fp
			fprintf(fp, responseReceived.C_String());
			fprintf(fp, "\n");
			if (contentOffset==-1)
			{
				if (eventCallback)
					eventCallback->OnResponse(R2RC_NO_CONTENT, responseReceived, contentOffset);
			}
			else
			{
				json_error_t error;

				json_t *root = json_loads(strstr(responseReceived.C_String() + contentOffset, "{"), JSON_REJECT_DUPLICATES | JSON_DISABLE_EOF_CHECK, &error);
				if (!root)
				{
					if (eventCallback)
						eventCallback->OnResponse(R2RC_BAD_JSON, responseReceived, contentOffset);
				}
				else
				{
					void *iter = json_object_iter(root);
					const char *firstKey = json_object_iter_key(iter);
					if (_stricmp(firstKey, "unauthorized")==0)
					{
						if (eventCallback)
							eventCallback->OnResponse(R2RC_UNAUTHORIZED, responseReceived, contentOffset);					
					}
					else if (_stricmp(firstKey, "itemNotFound")==0)
					{
						if (eventCallback)
							eventCallback->OnResponse(R2RC_404_NOT_FOUND, responseReceived, contentOffset);					
					}
					else if (_stricmp(firstKey, "access")==0)
					{
						json_t *valAuthToken = json_object_get(json_object_get(json_object_get(root, "access"), "token"), "id");
						strcpy_s(X_Auth_Token, json_string_value(valAuthToken));

						json_t *valAccountNumber = json_object_get(json_object_get(json_object_get(json_object_get(root, "access"), "token"), "tenant"), "id");
						cloudAccountNumber = atoi(json_string_value(valAccountNumber));	

						if (reexecuteLastRequestOnAuth)
						{
							reexecuteLastRequestOnAuth=false;

							json_t *root2 = json_loads(__addOpLast_dataAsStr.C_String(), 0, &error);							
							AddOperation(__addOpLast_URL, __addOpLast_isPost, root2, true);							
						}
						else
						{
							if (eventCallback)
								eventCallback->OnResponse(R2RC_AUTHENTICATED, responseReceived, contentOffset);
						}

					}
					else if (_stricmp(firstKey, "domains")==0)
					{
						if (eventCallback)
							eventCallback->OnResponse(R2RC_GOT_DOMAINS, responseReceived, contentOffset);
					}
					else if (_stricmp(firstKey, "records")==0)
					{
						if (eventCallback)
							eventCallback->OnResponse(R2RC_GOT_RECORDS, responseReceived, contentOffset);
					}
					else if (_stricmp(firstKey, "servers")==0)
					{
						if (eventCallback)
							eventCallback->OnResponse(R2RC_GOT_SERVERS, responseReceived, contentOffset);
					}
					else if (_stricmp(firstKey, "images")==0)
					{
						if (eventCallback)
							eventCallback->OnResponse(R2RC_GOT_IMAGES, responseReceived, contentOffset);
					}
					else if (_stricmp(firstKey, "message")==0)
					{
						const char *message = json_string_value(json_object_iter_value(iter));

						if (strcmp(message, "Invalid authentication token. Please renew.")==0)
						{
							// Sets reexecuteLastRequestOnAuth to true
							// After authenticate completes, will rerun the last run command
							Reauthenticate();
						}
						else
						{
							if (eventCallback)
								eventCallback->OnMessage(message, responseReceived, stringTransmitted, contentOffset);
						}
					}
					else
					{
						if (eventCallback)
							eventCallback->OnResponse(R2RC_UNKNOWN, responseReceived, contentOffset);
					}
				}

				json_decref(root);
			}
		}
		else
		{
			if (eventCallback)
				eventCallback->OnEmptyResponse(stringTransmitted);
		}
	}
}
void Rackspace2::SetEventCallback(Rackspace2EventCallback *callback)
{
	eventCallback=callback;
}
int Rackspace2::GetCloudAccountNumber(void) const
{
	return cloudAccountNumber;
}
const char *Rackspace2::GetAuthToken(void) const
{
	return (const char*) &X_Auth_Token;
}
void Rackspace2::Reauthenticate(void)
{
	reexecuteLastRequestOnAuth=true;
	AuthenticateInt(lastAuthenticationURL.C_String(), lastRackspaceCloudUsername.C_String(), lastApiAccessKey.C_String());
}
void Rackspace2::AuthenticateInt(const char *authenticationURL, const char *rackspaceCloudUsername, const char *apiAccessKey)
{
	json_t *json_credentials = json_object();
	json_object_set(json_credentials, "username", json_string(rackspaceCloudUsername));
	json_object_set(json_credentials, "apiKey", json_string(apiAccessKey));
	json_t *json_auth = json_object();
	json_object_set(json_auth, "RAX-KSKEY:apiKeyCredentials", json_credentials);
	json_t *json_root = json_object();
	json_object_set(json_root, "auth", json_auth);

	RakString URL = authenticationURL;
	RakString command = "/tokens";
	URL += command;

	AddOperation(URL, OT_POST, json_root, false);
}
void Rackspace2::Authenticate(const char *authenticationURL, const char *rackspaceCloudUsername, const char *apiAccessKey)
{
	lastAuthenticationURL=authenticationURL;
	lastRackspaceCloudUsername=rackspaceCloudUsername;
	lastApiAccessKey=apiAccessKey;
	AuthenticateInt(authenticationURL, rackspaceCloudUsername,apiAccessKey);
}
void Rackspace2::AddOperation(SLNet::RakString URL, OpType opType, json_t *data, bool setAuthToken)
{
	if (tcp==0)
	{
		tcp = SLNet::OP_NEW<TCPInterface>(_FILE_AND_LINE_);

		if (tcp->Start(0, 0, 8)==false)
		{
			if (eventCallback)
				eventCallback->OnTCPFailure();
		}

		httpConnection2 = SLNet::OP_NEW<HTTPConnection2>(_FILE_AND_LINE_);

		tcp->AttachPlugin(httpConnection2);
	}

	RakString authURLHeader, authURLDomain, authURLPath;
	URL.SplitURI(authURLHeader,authURLDomain,authURLPath);
	char *jsonStr = "";
	if (data)
		jsonStr = json_dumps(data, 0);
	RakString requestStr;
	RakString extraBody;
	if (setAuthToken)
	{
		RakAssert(X_Auth_Token[0]);
		// Test expired token
		//strcpy_s(X_Auth_Token, "fd6ad67c-fbd3-4b35-94e2-059b6090998e");
		extraBody.Set("Accept: application/json\r\nX-Auth-Token: %s", X_Auth_Token);

		__addOpLast_URL = URL;
		__addOpLast_isPost = opType;
		__addOpLast_dataAsStr = jsonStr;
	}
	else
	{
		extraBody = "Accept: application/json";
	}

	if (opType==OT_POST)
		requestStr = RakString::FormatForPOST(URL, "application/json", jsonStr, extraBody);
	else if (opType==OT_GET)
		requestStr = RakString::FormatForGET(URL, extraBody);
	else  if (opType==OT_DELETE)
		requestStr = RakString::FormatForDELETE(URL, extraBody);
	else
		requestStr = RakString::FormatForPUT(URL, "application/json", jsonStr, extraBody);

	bool b = httpConnection2->TransmitRequest(requestStr,authURLDomain, 443, true);
	if (!b)
	{
		if (eventCallback)
			eventCallback->OnTransmissionFailed(httpConnection2, requestStr, authURLDomain);
	}
	if (data)
	{
		free(jsonStr);
		json_decref(data);
	}
}

