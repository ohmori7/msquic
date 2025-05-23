/*++

    Copyright (c) Microsoft Corporation.
    Licensed under the MIT License.

Abstract:

    Tests handshake related features and functionality.

--*/

#include "precomp.h"
#ifdef QUIC_CLOG
#include "HandshakeTest.cpp.clog.h"
#endif

char CurrentWorkingDirectory[MAX_PATH + 1];

QUIC_TEST_DATAPATH_HOOKS DatapathHooks::FuncTable = {
    DatapathHooks::CreateCallback,
    DatapathHooks::GetLocalAddressCallback,
    DatapathHooks::GetRemoteAddressCallback,
    DatapathHooks::ReceiveCallback,
    DatapathHooks::SendCallback
};
DatapathHooks* DatapathHooks::Instance;

void QuicTestInitialize()
{
    DatapathHooks::Instance = new(std::nothrow) DatapathHooks;
}

void QuicTestUninitialize()
{
    delete DatapathHooks::Instance;
    DatapathHooks::Instance = nullptr;
}

void
QuicTestPrimeResumption(
    _In_ QUIC_ADDRESS_FAMILY QuicAddrFamily,
    _In_ MsQuicRegistration& Registration,
    _In_ MsQuicConfiguration& ServerConfiguration,
    _In_ MsQuicConfiguration& ClientConfiguration,
    _Out_ QUIC_BUFFER** ResumptionTicket
    )
{
    TestScopeLogger logScope("PrimeResumption");
    *ResumptionTicket = nullptr;

    struct PrimeResumption {
        CxPlatEvent ShutdownEvent;
        MsQuicConnection* Connection {nullptr};

        static QUIC_STATUS ConnCallback(_In_ MsQuicConnection* Conn, _In_opt_ void* Context, _Inout_ QUIC_CONNECTION_EVENT* Event) {
            PrimeResumption* Ctx = static_cast<PrimeResumption*>(Context);
            Ctx->Connection = Conn;
            if (Event->Type == QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE) {
                Ctx->Connection = nullptr;
                Ctx->ShutdownEvent.Set();
            } else if (Event->Type == QUIC_CONNECTION_EVENT_CONNECTED) {
                MsQuic->ConnectionSendResumptionTicket(Conn->Handle, QUIC_SEND_RESUMPTION_FLAG_FINAL, 0, nullptr);
            }
            return QUIC_STATUS_SUCCESS;
        }
    };

    PrimeResumption Context;
    MsQuicAutoAcceptListener Listener(Registration, ServerConfiguration, PrimeResumption::ConnCallback, &Context);
    TEST_TRUE(Listener.IsValid());

    QuicAddr ServerLocalAddr;
    TEST_QUIC_SUCCEEDED(Listener.Start("MsQuicTest"));
    TEST_QUIC_SUCCEEDED(Listener.GetLocalAddr(ServerLocalAddr));

    {
        TestConnection Client(Registration);
        TEST_TRUE(Client.IsValid());

        if (UseDuoNic) {
            QuicAddr RemoteAddr{QuicAddrFamily, ServerLocalAddr.GetPort()};
            QuicAddrSetToDuoNic(&RemoteAddr.SockAddr);
            TEST_QUIC_SUCCEEDED(Client.SetRemoteAddr(RemoteAddr));
        }

        TEST_QUIC_SUCCEEDED(
            Client.Start(
                ClientConfiguration,
                QuicAddrGetFamily(&ServerLocalAddr.SockAddr),
                QUIC_LOCALHOST_FOR_AF(QuicAddrFamily),
                ServerLocalAddr.GetPort()));
        if (Client.WaitForConnectionComplete()) {
            TEST_TRUE(Client.GetIsConnected());
            *ResumptionTicket = Client.WaitForResumptionTicket();
            if (*ResumptionTicket == nullptr) {
                TEST_FAILURE("Failed to prime resumption ticket.");
            }
        }

        TEST_NOT_EQUAL(nullptr, Context.Connection);
        Client.Shutdown(QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT, 0);
        Context.Connection->Shutdown(0, QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT);
        Context.ShutdownEvent.WaitTimeout(2000);
    }
}

_Function_class_(NEW_CONNECTION_CALLBACK)
static
bool
QUIC_API
ListenerAcceptConnection(
    _In_ TestListener* Listener,
    _In_ HQUIC ConnectionHandle
    )
{
    ServerAcceptContext* AcceptContext = (ServerAcceptContext*)Listener->Context;
    *AcceptContext->NewConnection = new(std::nothrow) TestConnection(ConnectionHandle, (NEW_STREAM_CALLBACK_HANDLER)AcceptContext->NewStreamHandler);
    (*AcceptContext->NewConnection)->SetExpectedCustomTicketValidationResult(AcceptContext->ExpectedCustomTicketValidationResult);
    (*AcceptContext->NewConnection)->SetAsyncCustomValidationResult(AcceptContext->AsyncCustomCertValidation);
    if (AcceptContext->IsCustomCertValidationResultSet) {
        (*AcceptContext->NewConnection)->SetExpectedCustomValidationResult(AcceptContext->CustomCertValidationResult);
    }
    if (*AcceptContext->NewConnection == nullptr || !(*AcceptContext->NewConnection)->IsValid()) {
        TEST_FAILURE("Failed to accept new TestConnection.");
        delete *AcceptContext->NewConnection;
        *AcceptContext->NewConnection = nullptr;
        return false;
    }
    (*AcceptContext->NewConnection)->SetHasRandomLoss(Listener->GetHasRandomLoss());
    if (AcceptContext->ExpectedTransportCloseStatus != QUIC_STATUS_SUCCESS) {
        (*AcceptContext->NewConnection)->SetExpectedTransportCloseStatus(
            AcceptContext->ExpectedTransportCloseStatus);
    }
    if (AcceptContext->ExpectedClientCertValidationResultCount > 0) {
        for (unsigned i = 0; i < AcceptContext->ExpectedClientCertValidationResultCount; i++) {
            (*AcceptContext->NewConnection)->AddExpectedClientCertValidationResult(
                AcceptContext->ExpectedClientCertValidationResult[i]);
        }
    }
    if (AcceptContext->PeerCertEventReturnStatus != QUIC_STATUS_SUCCESS) {
        (*AcceptContext->NewConnection)->SetPeerCertEventReturnStatus(
            AcceptContext->PeerCertEventReturnStatus);
    }
    if (AcceptContext->TlsSecrets != NULL) {
        auto Status = (*AcceptContext->NewConnection)->SetTlsSecrets(AcceptContext->TlsSecrets);
        if (QUIC_FAILED(Status)) {
            TEST_FAILURE("SetParam(QUIC_PARAM_CONN_TLS_SECRETS) returned 0x%x", Status);
            return false;
        }
    }
    CxPlatEventSet(AcceptContext->NewConnectionReady);
    return true;
}

void
QuicTestConnect(
    _In_ int Family,
    _In_ bool ServerStatelessRetry,
    _In_ bool ClientUsesOldVersion,
    _In_ bool MultipleALPNs,
    _In_ bool GreaseQuicBitEnabled,
    _In_ QUIC_TEST_ASYNC_CONFIG_MODE AsyncConfiguration,
    _In_ bool MultiPacketClientInitial,
    _In_ QUIC_TEST_RESUMPTION_MODE SessionResumption,
    _In_ uint8_t RandomLossPercentage
    )
{
    QUIC_ADDRESS_FAMILY QuicAddrFamily = (Family == 4) ? QUIC_ADDRESS_FAMILY_INET : QUIC_ADDRESS_FAMILY_INET6;
    MsQuicRegistration Registration;
    bool AsyncTicketValidation = SessionResumption == QUIC_TEST_RESUMPTION_ENABLED_ASYNC ||
                                 SessionResumption == QUIC_TEST_RESUMPTION_REJECTED_BY_SERVER_APP_ASYNC;
    TEST_TRUE(Registration.IsValid());

    MsQuicAlpn Alpn1("MsQuicTest");
    MsQuicAlpn Alpn2("MsQuicTest2", "MsQuicTest");

    QUIC_TLS_SECRETS ClientSecrets{}, ServerSecrets{};

    MsQuicSettings Settings;
    Settings.SetPeerBidiStreamCount(4);
    Settings.SetGreaseQuicBitEnabled(GreaseQuicBitEnabled);
    if (RandomLossPercentage != 0) {
        Settings.SetIdleTimeoutMs(30000);
        Settings.SetDisconnectTimeoutMs(30000);
        Settings.SetInitialRttMs(50);
    } else {
        Settings.SetIdleTimeoutMs(10000);
    }
    if (SessionResumption != QUIC_TEST_RESUMPTION_DISABLED) {
        Settings.SetServerResumptionLevel(QUIC_SERVER_RESUME_ONLY);
    }

    MsQuicConfiguration ServerConfiguration(Registration, Alpn2, Settings, ServerSelfSignedCredConfig);
    TEST_TRUE(ServerConfiguration.IsValid());

    MsQuicCredentialConfig ClientCredConfig;
    MsQuicConfiguration ClientConfiguration(Registration, Alpn1, Settings, ClientCredConfig);
    TEST_TRUE(ClientConfiguration.IsValid());

    QUIC_TICKET_KEY_CONFIG GoodKey;
    CxPlatZeroMemory(&GoodKey, sizeof(GoodKey));
    GoodKey.MaterialLength = 64;

    QUIC_TICKET_KEY_CONFIG BadKey;
    CxPlatZeroMemory(&BadKey, sizeof(BadKey));
    BadKey.MaterialLength = 64;
    BadKey.Material[0] = 0xFF;

    if (SessionResumption == QUIC_TEST_RESUMPTION_REJECTED) {
        TEST_QUIC_SUCCEEDED(ServerConfiguration.SetTicketKey(&GoodKey));
    }

    QUIC_BUFFER* ResumptionTicket = nullptr;
    if (SessionResumption != QUIC_TEST_RESUMPTION_DISABLED) {
        QuicTestPrimeResumption(
            QuicAddrFamily,
            Registration,
            ServerConfiguration,
            ClientConfiguration,
            &ResumptionTicket);
        if (!ResumptionTicket) {
            return;
        }
    }

    StatelessRetryHelper RetryHelper(ServerStatelessRetry);
    PrivateTransportHelper TpHelper(MultiPacketClientInitial, !!ResumptionTicket);
    RandomLossHelper LossHelper(RandomLossPercentage);

    {
        if (SessionResumption == QUIC_TEST_RESUMPTION_REJECTED) {
            TEST_QUIC_SUCCEEDED(ServerConfiguration.SetTicketKey(&BadKey));
        }
        TestListener Listener(
            Registration,
            ListenerAcceptConnection,
            (AsyncConfiguration ? (HQUIC)nullptr : (HQUIC)ServerConfiguration));
        TEST_TRUE(Listener.IsValid());
        Listener.SetHasRandomLoss(RandomLossPercentage != 0);

        QuicAddr ServerLocalAddr(QuicAddrFamily);
        TEST_QUIC_SUCCEEDED(Listener.Start(MultipleALPNs ? Alpn2 : Alpn1, &ServerLocalAddr.SockAddr));
        TEST_QUIC_SUCCEEDED(Listener.GetLocalAddr(ServerLocalAddr));

        {
            UniquePtr<TestConnection> Server;
            ServerAcceptContext ServerAcceptCtx((TestConnection**)&Server);
            if (AsyncTicketValidation) {
                ServerAcceptCtx.ExpectedCustomTicketValidationResult = QUIC_STATUS_PENDING;
            } else {
                if (SessionResumption == QUIC_TEST_RESUMPTION_ENABLED) {
                    ServerAcceptCtx.ExpectedCustomTicketValidationResult = QUIC_STATUS_SUCCESS;
                } else if (SessionResumption == QUIC_TEST_RESUMPTION_REJECTED_BY_SERVER_APP) {
                    ServerAcceptCtx.ExpectedCustomTicketValidationResult = QUIC_STATUS_INTERNAL_ERROR;
                }
            }

            Listener.Context = &ServerAcceptCtx;

            {
                TestConnection Client(Registration);
                TEST_TRUE(Client.IsValid());
                Client.SetSslKeyLogFilePath();
                Client.SetHasRandomLoss(RandomLossPercentage != 0);

                if (ClientUsesOldVersion) {
                    TEST_QUIC_SUCCEEDED(
                        Client.SetQuicVersion(OLD_SUPPORTED_VERSION));
                }

                if (MultiPacketClientInitial) {
                    TEST_QUIC_SUCCEEDED(
                        Client.SetTestTransportParameter(&TpHelper));
                }

                if (SessionResumption != QUIC_TEST_RESUMPTION_DISABLED) {
                    Client.SetResumptionTicket(ResumptionTicket);
                    CXPLAT_FREE(ResumptionTicket, QUIC_POOL_TEST);
                    Client.SetExpectedResumed(SessionResumption == QUIC_TEST_RESUMPTION_ENABLED ||
                                              SessionResumption == QUIC_TEST_RESUMPTION_ENABLED_ASYNC);
                }

                if (UseDuoNic) {
                    QuicAddr RemoteAddr{QuicAddrGetFamily(&ServerLocalAddr.SockAddr), ServerLocalAddr.GetPort()};
                    QuicAddrSetToDuoNic(&RemoteAddr.SockAddr);
                    TEST_QUIC_SUCCEEDED(Client.SetRemoteAddr(RemoteAddr));
                }

                TEST_QUIC_SUCCEEDED(
                    Client.Start(
                        ClientConfiguration,
                        QuicAddrFamily,
                        QUIC_LOCALHOST_FOR_AF(QuicAddrFamily),
                        ServerLocalAddr.GetPort()));

                if (AsyncConfiguration || AsyncTicketValidation) {
                    if (!CxPlatEventWaitWithTimeout(ServerAcceptCtx.NewConnectionReady, TestWaitTimeout)) {
                        TEST_FAILURE("Timed out waiting for server accept.");
                    } else if (Server == nullptr) {
                        TEST_FAILURE("Failed to accept server connection.");
                    } else {
                        if (AsyncConfiguration) {
                            if (AsyncConfiguration == QUIC_TEST_ASYNC_CONFIG_DELAYED) {
                                CxPlatSleep(1000);
                            }
                            TEST_QUIC_SUCCEEDED(
                                Server->SetConfiguration(ServerConfiguration));
                        }
                        if (AsyncTicketValidation) {
                            CxPlatSleep(1000);
                            TEST_QUIC_SUCCEEDED(Server->SetCustomTicketValidationResult(SessionResumption == QUIC_TEST_RESUMPTION_ENABLED_ASYNC));
                        }
                    }
                }

                if (!Client.WaitForConnectionComplete()) {
                    return;
                }
                TEST_TRUE(Client.GetIsConnected());

                // After handshake, check and see if we have cached the TTL of the handshake packet.
                if (QuitTestIsFeatureSupported(CXPLAT_DATAPATH_FEATURE_TTL)) {
                    TEST_TRUE(Client.GetStatistics().HandshakeHopLimitTTL > 0);
                } else {
                    TEST_EQUAL(Client.GetStatistics().HandshakeHopLimitTTL, 0);
                }

                TEST_NOT_EQUAL(nullptr, Server);
                Server->SetSslKeyLogFilePath();
                if (!Server->WaitForConnectionComplete()) {
                    return;
                }
                TEST_TRUE(Server->GetIsConnected());

                ClientSecrets = Client.GetTlsSecrets();
                ServerSecrets = Server->GetTlsSecrets();

                TEST_EQUAL(
                    ServerSecrets.IsSet.ClientRandom,
                    ClientSecrets.IsSet.ClientRandom);
                TEST_TRUE(
                    !memcmp(
                        ServerSecrets.ClientRandom,
                        ClientSecrets.ClientRandom,
                        sizeof(ServerSecrets.ClientRandom)));

                TEST_EQUAL(ServerSecrets.SecretLength, ClientSecrets.SecretLength);
                TEST_TRUE(ServerSecrets.SecretLength <= QUIC_TLS_SECRETS_MAX_SECRET_LEN);

                TEST_EQUAL(
                    ServerSecrets.IsSet.ClientHandshakeTrafficSecret,
                    ClientSecrets.IsSet.ClientHandshakeTrafficSecret);
                TEST_TRUE(
                    !memcmp(
                        ServerSecrets.ClientHandshakeTrafficSecret,
                        ClientSecrets.ClientHandshakeTrafficSecret,
                        ServerSecrets.SecretLength));

                TEST_EQUAL(
                    ServerSecrets.IsSet.ServerHandshakeTrafficSecret,
                    ClientSecrets.IsSet.ServerHandshakeTrafficSecret);
                TEST_TRUE(
                    !memcmp(
                        ServerSecrets.ServerHandshakeTrafficSecret,
                        ClientSecrets.ServerHandshakeTrafficSecret,
                        ServerSecrets.SecretLength));

                TEST_EQUAL(
                    ServerSecrets.IsSet.ClientTrafficSecret0,
                    ClientSecrets.IsSet.ClientTrafficSecret0);
                TEST_TRUE(
                    !memcmp(
                        ServerSecrets.ClientTrafficSecret0,
                        ClientSecrets.ClientTrafficSecret0,
                        ServerSecrets.SecretLength));

                TEST_EQUAL(
                    ServerSecrets.IsSet.ServerTrafficSecret0,
                    ClientSecrets.IsSet.ServerTrafficSecret0);
                TEST_TRUE(
                    !memcmp(
                        ServerSecrets.ServerTrafficSecret0,
                        ClientSecrets.ServerTrafficSecret0,
                        ServerSecrets.SecretLength));

                uint8_t ClientOrigCid[32], ServerOrigCid[32];
                uint32_t ClientOrigCidLen, ServerOrigCidLen;
                TEST_QUIC_SUCCEEDED(Client.GetOrigDestCid(ClientOrigCid, ClientOrigCidLen));
                TEST_QUIC_SUCCEEDED(Server->GetOrigDestCid(ServerOrigCid, ServerOrigCidLen));
                TEST_EQUAL(ClientOrigCidLen, ServerOrigCidLen);
                TEST_TRUE(!memcmp(ClientOrigCid, ServerOrigCid, ClientOrigCidLen));

                if (ClientUsesOldVersion) {
                    TEST_EQUAL(Server->GetQuicVersion(), OLD_SUPPORTED_VERSION);
                } else {
                    TEST_EQUAL(Server->GetQuicVersion(), LATEST_SUPPORTED_VERSION);
                }

                if (ServerStatelessRetry) {
                    TEST_TRUE(Client.GetStatistics().StatelessRetry);
                }

                if (SessionResumption == QUIC_TEST_RESUMPTION_ENABLED ||
                    SessionResumption == QUIC_TEST_RESUMPTION_ENABLED_ASYNC) {
                    TEST_TRUE(Client.GetResumed());
                    TEST_TRUE(Server->GetResumed());
                } else if (SessionResumption == QUIC_TEST_RESUMPTION_REJECTED ||
                           SessionResumption == QUIC_TEST_RESUMPTION_REJECTED_BY_SERVER_APP ||
                           SessionResumption == QUIC_TEST_RESUMPTION_REJECTED_BY_SERVER_APP_ASYNC) {
                    TEST_FALSE(Client.GetResumed());
                    TEST_FALSE(Server->GetResumed());
                }

                TEST_EQUAL(
                    Server->GetPeerBidiStreamCount(),
                    Client.GetLocalBidiStreamCount());

                if (GreaseQuicBitEnabled) {
                    TEST_TRUE(Client.GetStatistics().GreaseBitNegotiated);
                    TEST_TRUE(Server->GetStatistics().GreaseBitNegotiated);
                }

                if (RandomLossPercentage == 0) {
                    //
                    // Don't worry about graceful shutdown if we have random
                    // loss. It will likely just result in the maximum wait
                    // timeout, causing the test to run longer.
                    //
                    Client.Shutdown(QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, QUIC_TEST_NO_ERROR);
                    if (!Client.WaitForShutdownComplete()) {
                        return;
                    }

                    TEST_FALSE(Client.GetPeerClosed());
                    TEST_FALSE(Client.GetTransportClosed());
                }
            }

            if (RandomLossPercentage == 0) {
                TEST_TRUE(Server->GetPeerClosed());
                TEST_EQUAL(Server->GetPeerCloseErrorCode(), QUIC_TEST_NO_ERROR);
            } else {
                Server->Shutdown(QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT, 0);
            }
        }
    }
}

struct RebindContext {
    bool Connected {false};
    CxPlatEvent HandshakeCompleteEvent;
    CxPlatEvent PeerAddrChangedEvent;
    QuicAddr PeerAddr;
    static QUIC_STATUS ConnCallback(_In_ MsQuicConnection*, _In_opt_ void* Context, _Inout_ QUIC_CONNECTION_EVENT* Event) {
        auto This = static_cast<RebindContext*>(Context);
        if (Event->Type == QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE) {
            This->PeerAddrChangedEvent.Set();
            This->HandshakeCompleteEvent.Set();
        } else if (Event->Type == QUIC_CONNECTION_EVENT_CONNECTED) {
            This->Connected = true;
            This->HandshakeCompleteEvent.Set();
        } else if (Event->Type == QUIC_CONNECTION_EVENT_PEER_ADDRESS_CHANGED) {
            This->PeerAddr.SockAddr = *Event->PEER_ADDRESS_CHANGED.Address;
            This->PeerAddrChangedEvent.Set();
        }
        return QUIC_STATUS_SUCCESS;
    }
};

void
QuicTestNatPortRebind(
    _In_ int Family,
    _In_ uint16_t KeepAlivePaddingSize
    )
{
    RebindContext Context;
    MsQuicRegistration Registration(true);
    TEST_TRUE(Registration.IsValid());

    MsQuicConfiguration ServerConfiguration(Registration, "MsQuicTest", ServerSelfSignedCredConfig);
    TEST_TRUE(ServerConfiguration.IsValid());

    MsQuicCredentialConfig ClientCredConfig;
    MsQuicConfiguration ClientConfiguration(Registration, "MsQuicTest", ClientCredConfig);
    TEST_TRUE(ClientConfiguration.IsValid());

    MsQuicAutoAcceptListener Listener(Registration, ServerConfiguration, RebindContext::ConnCallback, &Context);
    TEST_QUIC_SUCCEEDED(Listener.GetInitStatus());
    QUIC_ADDRESS_FAMILY QuicAddrFamily = (Family == 4) ? QUIC_ADDRESS_FAMILY_INET : QUIC_ADDRESS_FAMILY_INET6;
    QuicAddr ServerLocalAddr(QuicAddrFamily);
    TEST_QUIC_SUCCEEDED(Listener.Start("MsQuicTest", &ServerLocalAddr.SockAddr));
    TEST_QUIC_SUCCEEDED(Listener.GetLocalAddr(ServerLocalAddr));

    MsQuicConnection Connection(Registration);
    TEST_QUIC_SUCCEEDED(Connection.GetInitStatus());

    TEST_QUIC_SUCCEEDED(Connection.Start(ClientConfiguration, ServerLocalAddr.GetFamily(), QUIC_TEST_LOOPBACK_FOR_AF(ServerLocalAddr.GetFamily()), ServerLocalAddr.GetPort()));
    TEST_TRUE(Connection.HandshakeCompleteEvent.WaitTimeout(TestWaitTimeout));
    TEST_TRUE(Context.HandshakeCompleteEvent.WaitTimeout(TestWaitTimeout));
    TEST_TRUE(Context.Connected);
    CxPlatSleep(10);

    QuicAddr OrigLocalAddr;
    TEST_QUIC_SUCCEEDED(Connection.GetLocalAddr(OrigLocalAddr));
    ReplaceAddressHelper AddrHelper(OrigLocalAddr.SockAddr);

    AddrHelper.IncrementPort();
    if (KeepAlivePaddingSize) {
        Connection.SetKeepAlivePadding(KeepAlivePaddingSize);
    }
    Connection.SetSettings(MsQuicSettings{}.SetKeepAlive(25));

    TEST_TRUE(Context.PeerAddrChangedEvent.WaitTimeout(1000))
    TEST_TRUE(QuicAddrCompare(&AddrHelper.New, &Context.PeerAddr.SockAddr));

    Connection.Shutdown(1);
}

void
QuicTestNatAddrRebind(
    _In_ int Family,
    _In_ uint16_t KeepAlivePaddingSize,
    _In_ bool RebindDatapathAddr
    )
{
    RebindContext Context;
    MsQuicRegistration Registration(true);
    TEST_TRUE(Registration.IsValid());

    MsQuicConfiguration ServerConfiguration(Registration, "MsQuicTest", ServerSelfSignedCredConfig);
    TEST_TRUE(ServerConfiguration.IsValid());

    MsQuicCredentialConfig ClientCredConfig;
    MsQuicConfiguration ClientConfiguration(Registration, "MsQuicTest", ClientCredConfig);
    TEST_TRUE(ClientConfiguration.IsValid());

    MsQuicAutoAcceptListener Listener(Registration, ServerConfiguration, RebindContext::ConnCallback, &Context);
    TEST_QUIC_SUCCEEDED(Listener.GetInitStatus());
    QUIC_ADDRESS_FAMILY QuicAddrFamily = (Family == 4) ? QUIC_ADDRESS_FAMILY_INET : QUIC_ADDRESS_FAMILY_INET6;
    QuicAddr ServerLocalAddr(QuicAddrFamily);
    TEST_QUIC_SUCCEEDED(Listener.Start("MsQuicTest", &ServerLocalAddr.SockAddr));
    TEST_QUIC_SUCCEEDED(Listener.GetLocalAddr(ServerLocalAddr));

    MsQuicConnection Connection(Registration);
    TEST_QUIC_SUCCEEDED(Connection.GetInitStatus());

    TEST_QUIC_SUCCEEDED(Connection.Start(ClientConfiguration, ServerLocalAddr.GetFamily(), QUIC_TEST_LOOPBACK_FOR_AF(ServerLocalAddr.GetFamily()), ServerLocalAddr.GetPort()));
    TEST_TRUE(Connection.HandshakeCompleteEvent.WaitTimeout(TestWaitTimeout));
    TEST_TRUE(Context.HandshakeCompleteEvent.WaitTimeout(TestWaitTimeout));
    TEST_TRUE(Context.Connected);
    CxPlatSleep(10);

    QuicAddr OrigLocalAddr;
    TEST_QUIC_SUCCEEDED(Connection.GetLocalAddr(OrigLocalAddr));
    ReplaceAddressHelper AddrHelper(OrigLocalAddr.SockAddr, OrigLocalAddr.SockAddr);

    if (RebindDatapathAddr) {
        QuicAddrFromString(UseDuoNic ? ((Family == AF_INET) ? "127.0.0.1" : "::1") : ((Family == AF_INET) ? "192.168.1.11" : "fc00::1:11"),
                           OrigLocalAddr.GetPort(),
                           &AddrHelper.New);
    } else {
        AddrHelper.IncrementAddr();
    }
    if (KeepAlivePaddingSize) {
        Connection.SetKeepAlivePadding(KeepAlivePaddingSize);
    }
    Connection.SetSettings(MsQuicSettings{}.SetKeepAlive(1));

    TEST_TRUE(Context.PeerAddrChangedEvent.WaitTimeout(1000))
    TEST_TRUE(QuicAddrCompare(&AddrHelper.New, &Context.PeerAddr.SockAddr));

    Connection.Shutdown(1);
}

void
QuicTestPathValidationTimeout(
    _In_ int Family
    )
{
    MsQuicRegistration Registration;
    TEST_TRUE(Registration.IsValid());

    MsQuicAlpn Alpn("MsQuicTest");

    MsQuicSettings Settings;
    Settings.SetIdleTimeoutMs(10000);

    MsQuicConfiguration ServerConfiguration(Registration, Alpn, Settings, ServerSelfSignedCredConfig);
    TEST_TRUE(ServerConfiguration.IsValid());

    MsQuicCredentialConfig ClientCredConfig;
    MsQuicConfiguration ClientConfiguration(Registration, Alpn, Settings, ClientCredConfig);
    TEST_TRUE(ClientConfiguration.IsValid());

    {
        TestListener Listener(Registration, ListenerAcceptConnection, ServerConfiguration);
        TEST_TRUE(Listener.IsValid());

        QUIC_ADDRESS_FAMILY QuicAddrFamily = (Family == 4) ? QUIC_ADDRESS_FAMILY_INET : QUIC_ADDRESS_FAMILY_INET6;
        QuicAddr ServerLocalAddr(QuicAddrFamily);
        TEST_QUIC_SUCCEEDED(Listener.Start(Alpn, &ServerLocalAddr.SockAddr));
        TEST_QUIC_SUCCEEDED(Listener.GetLocalAddr(ServerLocalAddr));

        {
            UniquePtr<TestConnection> Server;
            ServerAcceptContext ServerAcceptCtx((TestConnection**)&Server);
            Listener.Context = &ServerAcceptCtx;

            {
                TestConnection Client(Registration);
                TEST_TRUE(Client.IsValid());

                TEST_QUIC_SUCCEEDED(
                    Client.Start(
                        ClientConfiguration,
                        QuicAddrFamily,
                        QUIC_TEST_LOOPBACK_FOR_AF(QuicAddrFamily),
                        ServerLocalAddr.GetPort()));

                if (!Client.WaitForConnectionComplete()) {
                    return;
                }
                TEST_TRUE(Client.GetIsConnected());

                TEST_NOT_EQUAL(nullptr, Server);
                if (!Server->WaitForConnectionComplete()) {
                    return;
                }
                TEST_TRUE(Server->GetIsConnected());

                Server->SetExpectedTransportCloseStatus(QUIC_STATUS_CONNECTION_TIMEOUT);
                TEST_QUIC_SUCCEEDED(Server->SetDisconnectTimeout(1000)); // ms

                QuicAddr OrigLocalAddr;
                TEST_QUIC_SUCCEEDED(Client.GetLocalAddr(OrigLocalAddr));
                QuicAddr NewLocalAddr(OrigLocalAddr, 1);
                CxPlatSleep(200);

                ReplaceAddressThenDropHelper AddrHelper(OrigLocalAddr.SockAddr, NewLocalAddr.SockAddr, 1);
                TEST_FALSE(Client.GetIsShutdown());
                Client.SetKeepAlive(25);

                CxPlatSleep(200);
                Client.Shutdown(QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT, QUIC_TEST_NO_ERROR);
            }

            if (Server) {
                Server->WaitForShutdownComplete();
            }
        }
    }
}

void
QuicTestChangeMaxStreamID(
    _In_ int Family
    )
{
    MsQuicRegistration Registration;
    TEST_TRUE(Registration.IsValid());

    MsQuicAlpn Alpn("MsQuicTest");

    MsQuicSettings Settings;
    Settings.SetIdleTimeoutMs(10000);

    MsQuicConfiguration ServerConfiguration(Registration, Alpn, Settings, ServerSelfSignedCredConfig);
    TEST_TRUE(ServerConfiguration.IsValid());

    MsQuicCredentialConfig ClientCredConfig;
    MsQuicConfiguration ClientConfiguration(Registration, Alpn, Settings, ClientCredConfig);
    TEST_TRUE(ClientConfiguration.IsValid());

    {
        TestListener Listener(Registration, ListenerAcceptConnection, ServerConfiguration);
        TEST_TRUE(Listener.IsValid());

        QUIC_ADDRESS_FAMILY QuicAddrFamily = (Family == 4) ? QUIC_ADDRESS_FAMILY_INET : QUIC_ADDRESS_FAMILY_INET6;
        QuicAddr ServerLocalAddr(QuicAddrFamily);
        TEST_QUIC_SUCCEEDED(Listener.Start(Alpn, &ServerLocalAddr.SockAddr));
        TEST_QUIC_SUCCEEDED(Listener.GetLocalAddr(ServerLocalAddr));


        {
            UniquePtr<TestConnection> Server;
            ServerAcceptContext ServerAcceptCtx((TestConnection**)&Server);
            Listener.Context = &ServerAcceptCtx;

            {
                TestConnection Client(Registration);
                TEST_TRUE(Client.IsValid());

                TEST_QUIC_SUCCEEDED(
                    Client.Start(
                        ClientConfiguration,
                        QuicAddrFamily,
                        QUIC_TEST_LOOPBACK_FOR_AF(QuicAddrFamily),
                        ServerLocalAddr.GetPort()));

                if (!Client.WaitForConnectionComplete()) {
                    return;
                }
                TEST_TRUE(Client.GetIsConnected());

                TEST_NOT_EQUAL(nullptr, Server);
                if (!Server->WaitForConnectionComplete()) {
                    return;
                }
                TEST_TRUE(Server->GetIsConnected());

                TEST_EQUAL(
                    Server->GetPeerBidiStreamCount(),
                    Client.GetLocalBidiStreamCount());

                TEST_QUIC_SUCCEEDED(Client.SetPeerBidiStreamCount(101));
                TEST_EQUAL(101, Client.GetPeerBidiStreamCount());
                CxPlatSleep(100);
                TEST_EQUAL(101, Server->GetLocalBidiStreamCount());

                TEST_QUIC_SUCCEEDED(Server->SetPeerBidiStreamCount(100));
                TEST_EQUAL(100, Server->GetPeerBidiStreamCount());
                CxPlatSleep(100);
                TEST_EQUAL(100, Client.GetLocalBidiStreamCount());

                Client.Shutdown(QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, QUIC_TEST_NO_ERROR);
                if (!Client.WaitForShutdownComplete()) {
                    return;
                }

                TEST_FALSE(Client.GetPeerClosed());
                TEST_FALSE(Client.GetTransportClosed());
            }

            TEST_TRUE(Server->GetPeerClosed());
            TEST_EQUAL(Server->GetPeerCloseErrorCode(), QUIC_TEST_NO_ERROR);
        }
    }
}

void
QuicTestConnectAndIdle(
    _In_ bool EnableKeepAlive
    )
{
    MsQuicRegistration Registration;
    TEST_TRUE(Registration.IsValid());

    MsQuicAlpn Alpn("MsQuicTest");

    MsQuicSettings Settings;
    Settings.SetIdleTimeoutMs(3000);

    MsQuicConfiguration ServerConfiguration(Registration, Alpn, Settings, ServerSelfSignedCredConfig);
    TEST_TRUE(ServerConfiguration.IsValid());

    MsQuicCredentialConfig ClientCredConfig;
    MsQuicConfiguration ClientConfiguration(Registration, Alpn, Settings, ClientCredConfig);
    TEST_TRUE(ClientConfiguration.IsValid());

    {
        TestListener Listener(Registration, ListenerAcceptConnection, ServerConfiguration);
        TEST_TRUE(Listener.IsValid());
        TEST_QUIC_SUCCEEDED(Listener.Start(Alpn));

        QuicAddr ServerLocalAddr;
        TEST_QUIC_SUCCEEDED(Listener.GetLocalAddr(ServerLocalAddr));

        {
            UniquePtr<TestConnection> Server;
            ServerAcceptContext ServerAcceptCtx((TestConnection**)&Server);
            Listener.Context = &ServerAcceptCtx;

            {
                TestConnection Client(Registration);
                TEST_TRUE(Client.IsValid());

                if (!EnableKeepAlive) {
                    Client.SetExpectedTransportCloseStatus(QUIC_STATUS_CONNECTION_IDLE);
                }

                TEST_QUIC_SUCCEEDED(
                    Client.Start(
                        ClientConfiguration,
                        QUIC_ADDRESS_FAMILY_UNSPEC,
                        QUIC_TEST_LOOPBACK_FOR_AF(
                            QuicAddrGetFamily(&ServerLocalAddr.SockAddr)),
                        ServerLocalAddr.GetPort()));

                if (!Client.WaitForConnectionComplete()) {
                    return;
                }
                TEST_TRUE(Client.GetIsConnected());

                TEST_NOT_EQUAL(nullptr, Server);
                if (!EnableKeepAlive) {
                    Server->SetExpectedTransportCloseStatus(QUIC_STATUS_CONNECTION_IDLE);
                }
                if (!Server->WaitForConnectionComplete()) {
                    return;
                }
                TEST_TRUE(Server->GetIsConnected());

                if (EnableKeepAlive) {
                    TEST_QUIC_SUCCEEDED(Client.SetKeepAlive(1000));
                }

                CxPlatSleep(4000); // Wait for the first idle period to expire.

                if (EnableKeepAlive) {
                    TEST_FALSE(Client.GetIsShutdown());
                    TEST_FALSE(Server->GetIsShutdown());

                    Client.Shutdown(QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, QUIC_TEST_NO_ERROR);
                    if (!Client.WaitForShutdownComplete()) {
                        return;
                    }

                    TEST_FALSE(Client.GetPeerClosed());
                    TEST_FALSE(Client.GetTransportClosed());

#if !QUIC_SEND_FAKE_LOSS
                    TEST_TRUE(Server->GetPeerClosed());
                    TEST_EQUAL(Server->GetPeerCloseErrorCode(), QUIC_TEST_NO_ERROR);
#endif
                } else {
                    TEST_TRUE(Client.GetIsShutdown());
                    TEST_TRUE(Server->GetIsShutdown());
                    TEST_TRUE(Client.GetTransportClosed());
                    TEST_TRUE(Server->GetTransportClosed());
                }
            }
        }
    }
}

void
QuicTestCustomServerCertificateValidation(
    _In_ bool AcceptCert,
    _In_ bool AsyncValidation
    )
{
    MsQuicRegistration Registration;
    TEST_TRUE(Registration.IsValid());

    MsQuicAlpn Alpn("MsQuicTest");

    MsQuicSettings Settings;
    Settings.SetIdleTimeoutMs(3000);

    MsQuicConfiguration ServerConfiguration(Registration, Alpn, Settings, ServerSelfSignedCredConfig);
    TEST_TRUE(ServerConfiguration.IsValid());

    MsQuicCredentialConfig ClientCredConfig(QUIC_CREDENTIAL_FLAG_CLIENT | QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION | QUIC_CREDENTIAL_FLAG_INDICATE_CERTIFICATE_RECEIVED);
    MsQuicConfiguration ClientConfiguration(Registration, Alpn, Settings, ClientCredConfig);
    TEST_TRUE(ClientConfiguration.IsValid());

    {
        TestListener Listener(Registration, ListenerAcceptConnection, ServerConfiguration);
        TEST_TRUE(Listener.IsValid());
        TEST_QUIC_SUCCEEDED(Listener.Start(Alpn));

        QuicAddr ServerLocalAddr;
        TEST_QUIC_SUCCEEDED(Listener.GetLocalAddr(ServerLocalAddr));

        {
            UniquePtr<TestConnection> Server;
            ServerAcceptContext ServerAcceptCtx((TestConnection**)&Server);
            if (!AcceptCert) {
                ServerAcceptCtx.ExpectedTransportCloseStatus = QUIC_STATUS_BAD_CERTIFICATE;
            }
            Listener.Context = &ServerAcceptCtx;

            {
                TestConnection Client(Registration);
                TEST_TRUE(Client.IsValid());

                Client.SetSslKeyLogFilePath();
                Client.SetExpectedCustomValidationResult(AcceptCert);
                Client.SetAsyncCustomValidationResult(AsyncValidation);
                if (!AcceptCert) {
                    Client.SetExpectedTransportCloseStatus(QUIC_STATUS_BAD_CERTIFICATE);
                }

                TEST_QUIC_SUCCEEDED(
                    Client.Start(
                        ClientConfiguration,
                        QUIC_ADDRESS_FAMILY_UNSPEC,
                        QUIC_TEST_LOOPBACK_FOR_AF(
                            QuicAddrGetFamily(&ServerLocalAddr.SockAddr)),
                        ServerLocalAddr.GetPort()));

                if (AsyncValidation) {
                    CxPlatSleep(1000);
                    TEST_QUIC_SUCCEEDED(Client.SetCustomValidationResult(AcceptCert));
                }

                if (!Client.WaitForConnectionComplete()) {
                    return;
                }
                TEST_EQUAL(AcceptCert, Client.GetIsConnected());

                TEST_NOT_EQUAL(nullptr, Server);
                Server->SetSslKeyLogFilePath();
                if (!Server->WaitForConnectionComplete()) {
                    return;
                }
                TEST_EQUAL(AcceptCert, Server->GetIsConnected());
            }
        }
    }
}

void
NoOpStreamShutdownCallback(
    _In_ TestStream* Stream
    )
{
    UNREFERENCED_PARAMETER(Stream);
}

void
NewStreamCallbackTestFail(
    _In_ TestConnection* Connection,
    _In_ HQUIC StreamHandle,
    _In_ QUIC_STREAM_OPEN_FLAGS Flags
    )
{
    UNREFERENCED_PARAMETER(Connection);
    UNREFERENCED_PARAMETER(Flags);
    MsQuic->StreamClose(StreamHandle);
    TEST_FAILURE("Unexpected new Stream received");
}

void
QuicTestCustomClientCertificateValidation(
    _In_ bool AcceptCert,
    _In_ bool AsyncValidation
    )
{
    MsQuicRegistration Registration;
    TEST_TRUE(Registration.IsValid());

    MsQuicAlpn Alpn("MsQuicTest");

    MsQuicSettings Settings;
    Settings.SetPeerBidiStreamCount(1);
    Settings.SetIdleTimeoutMs(3000);

    MsQuicConfiguration ServerConfiguration(Registration, Alpn, Settings, ServerSelfSignedCredConfigClientAuth);
    TEST_TRUE(ServerConfiguration.IsValid());

    MsQuicConfiguration ClientConfiguration(Registration, Alpn, Settings, ClientCertCredConfig);
    TEST_TRUE(ClientConfiguration.IsValid());

    {
        TestListener Listener(Registration, ListenerAcceptConnection, ServerConfiguration);
        TEST_TRUE(Listener.IsValid());
        TEST_QUIC_SUCCEEDED(Listener.Start(Alpn));

        QuicAddr ServerLocalAddr;
        TEST_QUIC_SUCCEEDED(Listener.GetLocalAddr(ServerLocalAddr));

        {
            UniquePtr<TestConnection> Server;
            ServerAcceptContext ServerAcceptCtx((TestConnection**)&Server);
            if (!AcceptCert) {
                ServerAcceptCtx.ExpectedTransportCloseStatus = QUIC_STATUS_BAD_CERTIFICATE;
                ServerAcceptCtx.NewStreamHandler = (void*)NewStreamCallbackTestFail;
            }
            ServerAcceptCtx.AsyncCustomCertValidation = AsyncValidation;
            if (!AsyncValidation) {
                ServerAcceptCtx.IsCustomCertValidationResultSet = true;
                ServerAcceptCtx.CustomCertValidationResult = AcceptCert;
            }
            ServerAcceptCtx.AddExpectedClientCertValidationResult(QUIC_STATUS_CERT_UNTRUSTED_ROOT);
            Listener.Context = &ServerAcceptCtx;

            {
                TestConnection Client(Registration);
                TEST_TRUE(Client.IsValid());
                Client.SetSslKeyLogFilePath();

                if (!AcceptCert) {
                    Client.SetExpectedTransportCloseStatus(QUIC_STATUS_BAD_CERTIFICATE);
                }

                UniquePtr<TestStream> ClientStream(
                    TestStream::FromConnectionHandle(
                        Client.GetConnection(),
                        NoOpStreamShutdownCallback,
                        QUIC_STREAM_OPEN_FLAG_NONE));

                TEST_QUIC_SUCCEEDED(ClientStream->Start(QUIC_STREAM_START_FLAG_IMMEDIATE));

                TEST_QUIC_SUCCEEDED(
                    Client.Start(
                        ClientConfiguration,
                        QUIC_ADDRESS_FAMILY_UNSPEC,
                        QUIC_TEST_LOOPBACK_FOR_AF(
                            QuicAddrGetFamily(&ServerLocalAddr.SockAddr)),
                        ServerLocalAddr.GetPort()));

                if (!CxPlatEventWaitWithTimeout(ServerAcceptCtx.NewConnectionReady, TestWaitTimeout)) {
                    TEST_FAILURE("Timed out waiting for server accept.");
                }

                if (AsyncValidation) {
                    CxPlatSleep(2000);
                    TEST_QUIC_SUCCEEDED(Server->SetCustomValidationResult(AcceptCert));
                }

                if (!Client.WaitForConnectionComplete()) {
                    return;
                }
                // In all cases, the client "connects", but in the rejection case, it gets disconnected.
                TEST_TRUE(Client.GetIsConnected());

                TEST_NOT_EQUAL(nullptr, Server);
                Server->SetSslKeyLogFilePath();
                if (!Server->WaitForConnectionComplete()) {
                    return;
                }

                TEST_EQUAL(AcceptCert, Server->GetIsConnected());
            }
        }
    }
}

void
QuicTestShutdownDuringHandshake(
    _In_ bool ClientShutdown
    )
{
    MsQuicRegistration Registration;
    TEST_TRUE(Registration.IsValid());

    MsQuicAlpn Alpn("MsQuicTest");

    MsQuicSettings Settings;
    Settings.SetIdleTimeoutMs(3000);

    MsQuicConfiguration ServerConfiguration(Registration, Alpn, Settings, ServerSelfSignedCredConfig);
    TEST_TRUE(ServerConfiguration.IsValid());

    MsQuicCredentialConfig ClientCredConfig(QUIC_CREDENTIAL_FLAG_CLIENT | QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION | QUIC_CREDENTIAL_FLAG_INDICATE_CERTIFICATE_RECEIVED);
    MsQuicConfiguration ClientConfiguration(Registration, Alpn, Settings, ClientCredConfig);
    TEST_TRUE(ClientConfiguration.IsValid());

    {
        TestListener Listener(Registration, ListenerAcceptConnection, ServerConfiguration);
        TEST_TRUE(Listener.IsValid());
        TEST_QUIC_SUCCEEDED(Listener.Start(Alpn));

        QuicAddr ServerLocalAddr;
        TEST_QUIC_SUCCEEDED(Listener.GetLocalAddr(ServerLocalAddr));

        {
            UniquePtr<TestConnection> Server;
            ServerAcceptContext ServerAcceptCtx((TestConnection**)&Server);
            ServerAcceptCtx.ExpectedTransportCloseStatus = QUIC_STATUS_USER_CANCELED;
            Listener.Context = &ServerAcceptCtx;

            {
                TestConnection Client(Registration);
                TEST_TRUE(Client.IsValid());

                Client.SetSslKeyLogFilePath();
                Client.SetExpectedCustomValidationResult(TRUE);
                Client.SetAsyncCustomValidationResult(TRUE);
                Client.SetExpectedTransportCloseStatus(QUIC_STATUS_USER_CANCELED);

                TEST_QUIC_SUCCEEDED(
                    Client.Start(
                        ClientConfiguration,
                        QUIC_ADDRESS_FAMILY_UNSPEC,
                        QUIC_TEST_LOOPBACK_FOR_AF(
                            QuicAddrGetFamily(&ServerLocalAddr.SockAddr)),
                        ServerLocalAddr.GetPort()));

                CxPlatSleep(1000);

                TEST_NOT_EQUAL(nullptr, Server);
                Server->SetSslKeyLogFilePath();

                //
                // By now, the handshake is waiting for custom certificate validation.
                //
                if (ClientShutdown) {
                    Client.Shutdown(QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, QUIC_TEST_NO_ERROR);

                    if (!Client.WaitForShutdownComplete()) {
                        return;
                    }

                    if (!Server->WaitForConnectionComplete()) {
                        return;
                    }

                    //
                    // server is not allowed to respond to 1-RTT packets, so it will only
                    // receive Handshake CONNECTION_CLOSE which cannot bear app error code
                    //
                    TEST_EQUAL(TRUE, Server->GetTransportClosed());
                } else {
                    Server->Shutdown(QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, QUIC_TEST_NO_ERROR);

                    if (!Server->WaitForShutdownComplete()) {
                        return;
                    }

                    if (!Client.WaitForConnectionComplete()) {
                        return;
                    }

                    //
                    // We can assert neither transport nor peer closed because the behavior
                    // is platform dependent. Schannel provides 1-RTT read keys early enough to
                    // receive the app CONNECTION_CLOSE, but OpenSSL does not and leads to
                    // transport close.
                    //
                }
            }
        }
    }

}

void
QuicTestConnectUnreachable(
    _In_ int Family
    )
{
    MsQuicRegistration Registration;
    TEST_TRUE(Registration.IsValid());
    if (QuitTestIsFeatureSupported(CXPLAT_DATAPATH_FEATURE_RAW)) {
        return;
    }

    MsQuicAlpn Alpn("MsQuicTest");

    MsQuicSettings Settings;
    Settings.SetIdleTimeoutMs(3000);

    MsQuicConfiguration ServerConfiguration(Registration, Alpn, Settings, ServerSelfSignedCredConfig);
    TEST_TRUE(ServerConfiguration.IsValid());

    MsQuicCredentialConfig ClientCredConfig;
    MsQuicConfiguration ClientConfiguration(Registration, Alpn, Settings, ClientCredConfig);
    TEST_TRUE(ClientConfiguration.IsValid());

    {
        QUIC_ADDRESS_FAMILY QuicAddrFamily = (Family == 4) ? QUIC_ADDRESS_FAMILY_INET : QUIC_ADDRESS_FAMILY_INET6;

        TestConnection Client(Registration);
        TEST_TRUE(Client.IsValid());

        Client.SetExpectedTransportCloseStatus(QUIC_STATUS_UNREACHABLE);
        TEST_QUIC_SUCCEEDED(
            Client.Start(
                ClientConfiguration,
                QuicAddrFamily,
                QUIC_TEST_LOOPBACK_FOR_AF(QuicAddrFamily),
                TestUdpPortBase - 1));
        if (!Client.WaitForConnectionComplete()) {
            return;
        }

        TEST_FALSE(Client.GetIsConnected());
        TEST_TRUE(Client.GetTransportClosed());
    }
}

void
QuicTestConnectInvalidAddress(
    )
{
    MsQuicRegistration Registration;
    TEST_TRUE(Registration.IsValid());

    MsQuicAlpn Alpn("MsQuicTest");

    MsQuicSettings Settings;
    Settings.SetIdleTimeoutMs(3000);

    MsQuicCredentialConfig ClientCredConfig;
    MsQuicConfiguration ClientConfiguration(Registration, Alpn, Settings, ClientCredConfig);
    TEST_TRUE(ClientConfiguration.IsValid());

    {
        TestConnection Client(Registration);
        TEST_TRUE(Client.IsValid());

        QuicAddr LocalAddr{QUIC_ADDRESS_FAMILY_INET, true};
        if (UseDuoNic) {
            QuicAddrSetToDuoNic(&LocalAddr.SockAddr);
        }
        LocalAddr.SetPort(TestUdpPortBase - 2);

        QuicAddr RemoteAddr{QUIC_ADDRESS_FAMILY_INET6, true};
        if (UseDuoNic) {
            QuicAddrSetToDuoNic(&RemoteAddr.SockAddr);
        }
        RemoteAddr.SetPort(TestUdpPortBase - 1);

        TEST_QUIC_SUCCEEDED(Client.SetLocalAddr(LocalAddr));
        TEST_QUIC_SUCCEEDED(Client.SetRemoteAddr(RemoteAddr));

        Client.SetExpectedTransportCloseStatus(QUIC_STATUS_INVALID_ADDRESS);
        TEST_QUIC_SUCCEEDED(
            Client.Start(
                ClientConfiguration,
                QUIC_ADDRESS_FAMILY_INET6,
                QUIC_TEST_LOOPBACK_FOR_AF(QUIC_ADDRESS_FAMILY_INET6),
                TestUdpPortBase - 1));
        if (!Client.WaitForConnectionComplete()) {
            return;
        }

        TEST_FALSE(Client.GetIsConnected());
        TEST_TRUE(Client.GetTransportClosed());
    }
}

#ifdef QUIC_API_ENABLE_PREVIEW_FEATURES
void
QuicTestVersionNegotiation(
    _In_ int Family
    )
{
    const uint32_t ClientVersions[] = { 168430090ul, LATEST_SUPPORTED_VERSION }; // Random reserved version to force VN.
    const uint32_t ClientVersionsLength = ARRAYSIZE(ClientVersions);
    MsQuicRegistration Registration;
    TEST_TRUE(Registration.IsValid());

    MsQuicAlpn Alpn("MsQuicTest");

    MsQuicSettings Settings;
    Settings.SetIdleTimeoutMs(3000);

    MsQuicSettings ClientSettings;
    ClientSettings.SetIdleTimeoutMs(3000);

    MsQuicVersionSettings VersionSettings(ClientVersions, ClientVersionsLength);

    MsQuicConfiguration ServerConfiguration(Registration, Alpn, Settings, ServerSelfSignedCredConfig);
    TEST_TRUE(ServerConfiguration.IsValid());

    MsQuicCredentialConfig ClientCredConfig;
    MsQuicConfiguration ClientConfiguration(Registration, Alpn, ClientSettings, ClientCredConfig);
    TEST_TRUE(ClientConfiguration.IsValid());
    TEST_QUIC_SUCCEEDED(ClientConfiguration.SetVersionSettings(VersionSettings));

    ClearGlobalVersionListScope ClearVersionsScope;
    BOOLEAN Enabled = TRUE;
    TEST_QUIC_SUCCEEDED(
        MsQuic->SetParam(
            NULL,
            QUIC_PARAM_GLOBAL_VERSION_NEGOTIATION_ENABLED,
            sizeof(Enabled),
            &Enabled));

    {
        TestListener Listener(Registration, ListenerAcceptConnection, ServerConfiguration);
        TEST_TRUE(Listener.IsValid());
        TEST_QUIC_SUCCEEDED(Listener.Start(Alpn));

        QUIC_ADDRESS_FAMILY QuicAddrFamily = (Family == 4) ? QUIC_ADDRESS_FAMILY_INET : QUIC_ADDRESS_FAMILY_INET6;
        QuicAddr ServerLocalAddr;
        TEST_QUIC_SUCCEEDED(Listener.GetLocalAddr(ServerLocalAddr));

        {
            UniquePtr<TestConnection> Server;
            ServerAcceptContext ServerAcceptCtx((TestConnection**)&Server);
            Listener.Context = &ServerAcceptCtx;

            {
                TestConnection Client(Registration);
                TEST_TRUE(Client.IsValid());
                Client.SetSslKeyLogFilePath();

                TEST_QUIC_SUCCEEDED(
                    Client.Start(
                        ClientConfiguration,
                        QuicAddrFamily,
                        QUIC_TEST_LOOPBACK_FOR_AF(QuicAddrFamily),
                        ServerLocalAddr.GetPort()));
                if (!Client.WaitForConnectionComplete()) {
                    return;
                }

                TEST_TRUE(Client.GetIsConnected());
                TEST_TRUE(Client.GetStatistics().VersionNegotiation);
                TEST_EQUAL(Client.GetQuicVersion(), LATEST_SUPPORTED_VERSION);

                TEST_NOT_EQUAL(nullptr, Server);
                Server->SetSslKeyLogFilePath();
            }
        }
    }
}

struct ClearForcedRetryScope {
    ~ClearForcedRetryScope() {
        uint16_t value = 65;

        TEST_QUIC_SUCCEEDED(
            MsQuic->SetParam(
                NULL,
                QUIC_PARAM_GLOBAL_RETRY_MEMORY_PERCENT,
                sizeof(value),
                &value));
    }
};

void
QuicTestVersionNegotiationRetry(
    _In_ int Family
    )
{
    const uint32_t ClientVersions[] = { 168430090ul, LATEST_SUPPORTED_VERSION }; // Random reserved version to force VN.
    const uint32_t ClientVersionsLength = ARRAYSIZE(ClientVersions);
    const uint16_t RetryMemoryLimit = 0;

    MsQuicRegistration Registration;
    TEST_TRUE(Registration.IsValid());

    TEST_QUIC_SUCCEEDED(
        MsQuic->SetParam(
            NULL,
            QUIC_PARAM_GLOBAL_RETRY_MEMORY_PERCENT,
            sizeof(RetryMemoryLimit),
            &RetryMemoryLimit));

    ClearForcedRetryScope ClearForcedRetry;

    MsQuicAlpn Alpn("MsQuicTest");

    MsQuicSettings Settings;
    Settings.SetIdleTimeoutMs(3000);

    MsQuicSettings ClientSettings;
    ClientSettings.SetIdleTimeoutMs(3000);

    MsQuicVersionSettings VersionSettings(ClientVersions, ClientVersionsLength);

    MsQuicConfiguration ServerConfiguration(Registration, Alpn, Settings, ServerSelfSignedCredConfig);
    TEST_TRUE(ServerConfiguration.IsValid());

    MsQuicCredentialConfig ClientCredConfig;
    MsQuicConfiguration ClientConfiguration(Registration, Alpn, ClientSettings, ClientCredConfig);
    TEST_TRUE(ClientConfiguration.IsValid());
    TEST_QUIC_SUCCEEDED(ClientConfiguration.SetVersionSettings(VersionSettings));

    ClearGlobalVersionListScope ClearVersionsScope;
    BOOLEAN Enabled = TRUE;
    TEST_QUIC_SUCCEEDED(
        MsQuic->SetParam(
            NULL,
            QUIC_PARAM_GLOBAL_VERSION_NEGOTIATION_ENABLED,
            sizeof(Enabled),
            &Enabled));

    {
        TestListener Listener(Registration, ListenerAcceptConnection, ServerConfiguration);
        TEST_TRUE(Listener.IsValid());
        TEST_QUIC_SUCCEEDED(Listener.Start(Alpn));

        QUIC_ADDRESS_FAMILY QuicAddrFamily = (Family == 4) ? QUIC_ADDRESS_FAMILY_INET : QUIC_ADDRESS_FAMILY_INET6;
        QuicAddr ServerLocalAddr;
        TEST_QUIC_SUCCEEDED(Listener.GetLocalAddr(ServerLocalAddr));

        {
            UniquePtr<TestConnection> Server;
            ServerAcceptContext ServerAcceptCtx((TestConnection**)&Server);
            Listener.Context = &ServerAcceptCtx;

            {
                TestConnection Client(Registration);
                TEST_TRUE(Client.IsValid());
                Client.SetSslKeyLogFilePath();

                TEST_QUIC_SUCCEEDED(
                    Client.Start(
                        ClientConfiguration,
                        QuicAddrFamily,
                        QUIC_TEST_LOOPBACK_FOR_AF(QuicAddrFamily),
                        ServerLocalAddr.GetPort()));
                if (!Client.WaitForConnectionComplete()) {
                    return;
                }

                TEST_TRUE(Client.GetIsConnected());
                TEST_TRUE(Client.GetStatistics().VersionNegotiation);
                TEST_TRUE(Client.GetStatistics().StatelessRetry);
                TEST_EQUAL(Client.GetQuicVersion(), LATEST_SUPPORTED_VERSION);

                TEST_NOT_EQUAL(nullptr, Server);
                Server->SetSslKeyLogFilePath();
            }
        }
    }
}

void
QuicTestCompatibleVersionNegotiation(
    _In_ int Family,
    _In_ bool DisableVNEClient,
    _In_ bool DisableVNEServer
    )
{
    const uint32_t ClientVersions[] = { QUIC_VERSION_1_H, QUIC_VERSION_2_H };
    const uint32_t ServerVersions[] = { QUIC_VERSION_2_H, QUIC_VERSION_1_H };
    const uint32_t ClientVersionsLength = ARRAYSIZE(ClientVersions);
    const uint32_t ServerVersionsLength = ARRAYSIZE(ServerVersions);
    const uint32_t ExpectedSuccessVersion = QUIC_VERSION_2_H;
    const uint32_t ExpectedFailureVersion = QUIC_VERSION_1_H;

    ClearGlobalVersionListScope ClearVersionsScope;

    MsQuicSettings ClientSettings;
    ClientSettings.SetIdleTimeoutMs(3000);

    MsQuicVersionSettings ClientVersionSettings(ClientVersions, ClientVersionsLength);

    MsQuicSettings ServerSettings;
    ServerSettings.SetIdleTimeoutMs(3000);

    MsQuicVersionSettings ServerVersionsSettings(ServerVersions, ServerVersionsLength);

    TEST_QUIC_SUCCEEDED(
        MsQuic->SetParam(
            NULL,
            QUIC_PARAM_GLOBAL_VERSION_SETTINGS,
            sizeof(ServerVersionsSettings),
            &ServerVersionsSettings));

    BOOLEAN Value = !DisableVNEServer;
    TEST_QUIC_SUCCEEDED(
        MsQuic->SetParam(
            NULL,
            QUIC_PARAM_GLOBAL_VERSION_NEGOTIATION_ENABLED,
            sizeof(Value),
            &Value));

    MsQuicRegistration Registration;
    TEST_TRUE(Registration.IsValid());

    MsQuicAlpn Alpn("MsQuicTest");

    MsQuicConfiguration ServerConfiguration(Registration, Alpn, ServerSettings, ServerSelfSignedCredConfig);
    TEST_TRUE(ServerConfiguration.IsValid());

    MsQuicCredentialConfig ClientCredConfig;
    MsQuicConfiguration ClientConfiguration(Registration, Alpn, ClientSettings, ClientCredConfig);
    TEST_TRUE(ClientConfiguration.IsValid());
    TEST_QUIC_SUCCEEDED(ClientConfiguration.SetVersionSettings(ClientVersionSettings));
    TEST_QUIC_SUCCEEDED(ClientConfiguration.SetVersionNegotiationExtEnabled(!DisableVNEClient));

    {
        TestListener Listener(Registration, ListenerAcceptConnection, ServerConfiguration);
        TEST_TRUE(Listener.IsValid());

        QUIC_ADDRESS_FAMILY QuicAddrFamily = (Family == 4) ? QUIC_ADDRESS_FAMILY_INET : QUIC_ADDRESS_FAMILY_INET6;
        QuicAddr ServerLocalAddr(QuicAddrFamily);
        TEST_QUIC_SUCCEEDED(Listener.Start(Alpn, &ServerLocalAddr.SockAddr));
        TEST_QUIC_SUCCEEDED(Listener.GetLocalAddr(ServerLocalAddr));
        {
            UniquePtr<TestConnection> Server;
            ServerAcceptContext ServerAcceptCtx((TestConnection**)&Server);
            Listener.Context = &ServerAcceptCtx;

            {
                TestConnection Client(Registration);
                TEST_TRUE(Client.IsValid());
                Client.SetSslKeyLogFilePath();

                TEST_QUIC_SUCCEEDED(
                    Client.Start(
                        ClientConfiguration,
                        QuicAddrFamily,
                        QUIC_TEST_LOOPBACK_FOR_AF(QuicAddrFamily),
                        ServerLocalAddr.GetPort()));

                if (!Client.WaitForConnectionComplete()) {
                    return;
                }
                TEST_TRUE(Client.GetIsConnected());

                TEST_NOT_EQUAL(nullptr, Server);
                Server->SetSslKeyLogFilePath();
                if (!Server->WaitForConnectionComplete()) {
                    return;
                }
                TEST_TRUE(Server->GetIsConnected());

                if (DisableVNEClient || DisableVNEServer) {
                    TEST_EQUAL(Client.GetQuicVersion(), ExpectedFailureVersion);
                    TEST_EQUAL(Server->GetQuicVersion(), ExpectedFailureVersion);
                } else {
                    TEST_EQUAL(Client.GetQuicVersion(), ExpectedSuccessVersion);
                    TEST_EQUAL(Server->GetQuicVersion(), ExpectedSuccessVersion);
                }
                TEST_FALSE(Client.GetStatistics().VersionNegotiation);
            }
        }
    }
}

void
QuicTestCompatibleVersionNegotiationRetry(
    _In_ int Family
    )
{
    const uint32_t ClientVersions[] = { QUIC_VERSION_1_H, QUIC_VERSION_2_H };
    const uint32_t ServerVersions[] = { QUIC_VERSION_2_H, QUIC_VERSION_1_H };
    const uint32_t ClientVersionsLength = ARRAYSIZE(ClientVersions);
    const uint32_t ServerVersionsLength = ARRAYSIZE(ServerVersions);
    const uint32_t ExpectedSuccessVersion = QUIC_VERSION_2_H;
    const uint16_t RetryMemoryLimit = 0;

    MsQuicSettings ClientSettings;
    ClientSettings.SetIdleTimeoutMs(3000);

    MsQuicVersionSettings ClientVersionSettings(ClientVersions, ClientVersionsLength);

    MsQuicSettings ServerSettings;
    ServerSettings.SetIdleTimeoutMs(3000);

    MsQuicVersionSettings ServerVersionsSettings(ServerVersions, ServerVersionsLength);

    TEST_QUIC_SUCCEEDED(
        MsQuic->SetParam(
            NULL,
            QUIC_PARAM_GLOBAL_RETRY_MEMORY_PERCENT,
            sizeof(RetryMemoryLimit),
            &RetryMemoryLimit));
    ClearForcedRetryScope ClearForcedRetry;

    TEST_QUIC_SUCCEEDED(
        MsQuic->SetParam(
            NULL,
            QUIC_PARAM_GLOBAL_VERSION_SETTINGS,
            sizeof(ServerVersionsSettings),
            &ServerVersionsSettings));
    BOOLEAN Value = TRUE;
    TEST_QUIC_SUCCEEDED(
        MsQuic->SetParam(
            NULL,
            QUIC_PARAM_GLOBAL_VERSION_NEGOTIATION_ENABLED,
            sizeof(Value),
            &Value));
    ClearGlobalVersionListScope ClearVersionsScope;

    MsQuicRegistration Registration;
    TEST_TRUE(Registration.IsValid());

    MsQuicAlpn Alpn("MsQuicTest");

    MsQuicConfiguration ServerConfiguration(Registration, Alpn, ServerSettings, ServerSelfSignedCredConfig);
    TEST_TRUE(ServerConfiguration.IsValid());

    MsQuicCredentialConfig ClientCredConfig;
    MsQuicConfiguration ClientConfiguration(Registration, Alpn, ClientSettings, ClientCredConfig);
    TEST_TRUE(ClientConfiguration.IsValid());
    TEST_QUIC_SUCCEEDED(ClientConfiguration.SetVersionSettings(ClientVersionSettings));

    {
        TestListener Listener(Registration, ListenerAcceptConnection, ServerConfiguration);
        TEST_TRUE(Listener.IsValid());

        QUIC_ADDRESS_FAMILY QuicAddrFamily = (Family == 4) ? QUIC_ADDRESS_FAMILY_INET : QUIC_ADDRESS_FAMILY_INET6;
        QuicAddr ServerLocalAddr(QuicAddrFamily);
        TEST_QUIC_SUCCEEDED(Listener.Start(Alpn, &ServerLocalAddr.SockAddr));
        TEST_QUIC_SUCCEEDED(Listener.GetLocalAddr(ServerLocalAddr));
        {
            UniquePtr<TestConnection> Server;
            ServerAcceptContext ServerAcceptCtx((TestConnection**)&Server);
            Listener.Context = &ServerAcceptCtx;

            {
                TestConnection Client(Registration);
                TEST_TRUE(Client.IsValid());
                Client.SetSslKeyLogFilePath();

                TEST_QUIC_SUCCEEDED(
                    Client.Start(
                        ClientConfiguration,
                        QuicAddrFamily,
                        QUIC_TEST_LOOPBACK_FOR_AF(QuicAddrFamily),
                        ServerLocalAddr.GetPort()));

                if (!Client.WaitForConnectionComplete()) {
                    return;
                }
                TEST_TRUE(Client.GetIsConnected());

                TEST_NOT_EQUAL(nullptr, Server);
                Server->SetSslKeyLogFilePath();
                if (!Server->WaitForConnectionComplete()) {
                    return;
                }
                TEST_TRUE(Server->GetIsConnected());

                TEST_EQUAL(Client.GetQuicVersion(), ExpectedSuccessVersion);
                TEST_EQUAL(Server->GetQuicVersion(), ExpectedSuccessVersion);
                TEST_FALSE(Client.GetStatistics().VersionNegotiation);
                TEST_TRUE(Client.GetStatistics().StatelessRetry);
            }
        }
    }
}

void
QuicTestCompatibleVersionNegotiationDefaultServer(
    _In_ int Family,
    _In_ bool DisableVNEClient,
    _In_ bool DisableVNEServer
    )
{
    const uint32_t ClientVersions[] = { QUIC_VERSION_1_H, QUIC_VERSION_2_H };
    const uint32_t ClientVersionsLength = ARRAYSIZE(ClientVersions);
    const uint32_t ExpectedSuccessVersion = QUIC_VERSION_2_H;
    const uint32_t ExpectedFailureVersion = QUIC_VERSION_1_H;

    MsQuicSettings ClientSettings;
    ClientSettings.SetIdleTimeoutMs(3000);

    MsQuicVersionSettings ClientVersionSettings(ClientVersions, ClientVersionsLength);

    MsQuicSettings ServerSettings;
    ServerSettings.SetIdleTimeoutMs(3000);

    //
    // Enable the VNE for server at the global level.
    //
    BOOLEAN Value = !DisableVNEServer;
    TEST_QUIC_SUCCEEDED(
        MsQuic->SetParam(
            NULL,
            QUIC_PARAM_GLOBAL_VERSION_NEGOTIATION_ENABLED,
            sizeof(Value),
            &Value));
    ClearGlobalVersionListScope ClearVersionsScope;

    MsQuicRegistration Registration;
    TEST_TRUE(Registration.IsValid());

    MsQuicAlpn Alpn("MsQuicTest");

    MsQuicConfiguration ServerConfiguration(Registration, Alpn, ServerSettings, ServerSelfSignedCredConfig);
    TEST_TRUE(ServerConfiguration.IsValid());

    MsQuicCredentialConfig ClientCredConfig;
    MsQuicConfiguration ClientConfiguration(Registration, Alpn, ClientSettings, ClientCredConfig);
    TEST_TRUE(ClientConfiguration.IsValid());
    TEST_QUIC_SUCCEEDED(ClientConfiguration.SetVersionSettings(ClientVersionSettings));
    TEST_QUIC_SUCCEEDED(ClientConfiguration.SetVersionNegotiationExtEnabled(!DisableVNEClient));

    {
        TestListener Listener(Registration, ListenerAcceptConnection, ServerConfiguration);
        TEST_TRUE(Listener.IsValid());

        QUIC_ADDRESS_FAMILY QuicAddrFamily = (Family == 4) ? QUIC_ADDRESS_FAMILY_INET : QUIC_ADDRESS_FAMILY_INET6;
        QuicAddr ServerLocalAddr(QuicAddrFamily);
        TEST_QUIC_SUCCEEDED(Listener.Start(Alpn, &ServerLocalAddr.SockAddr));
        TEST_QUIC_SUCCEEDED(Listener.GetLocalAddr(ServerLocalAddr));
        {
            UniquePtr<TestConnection> Server;
            ServerAcceptContext ServerAcceptCtx((TestConnection**)&Server);
            Listener.Context = &ServerAcceptCtx;

            {
                TestConnection Client(Registration);
                TEST_TRUE(Client.IsValid());
                Client.SetSslKeyLogFilePath();

                TEST_QUIC_SUCCEEDED(
                    Client.Start(
                        ClientConfiguration,
                        QuicAddrFamily,
                        QUIC_TEST_LOOPBACK_FOR_AF(QuicAddrFamily),
                        ServerLocalAddr.GetPort()));

                if (!Client.WaitForConnectionComplete()) {
                    return;
                }
                TEST_TRUE(Client.GetIsConnected());

                TEST_NOT_EQUAL(nullptr, Server);
                Server->SetSslKeyLogFilePath();
                if (!Server->WaitForConnectionComplete()) {
                    return;
                }
                TEST_TRUE(Server->GetIsConnected());

                if (DisableVNEClient || DisableVNEServer) {
                    TEST_EQUAL(Client.GetQuicVersion(), ExpectedFailureVersion);
                    TEST_EQUAL(Server->GetQuicVersion(), ExpectedFailureVersion);
                } else {
                    TEST_EQUAL(Client.GetQuicVersion(), ExpectedSuccessVersion);
                    TEST_EQUAL(Server->GetQuicVersion(), ExpectedSuccessVersion);
                }
                TEST_FALSE(Client.GetStatistics().VersionNegotiation);
            }
        }
    }
}

void
QuicTestCompatibleVersionNegotiationDefaultClient(
    _In_ int Family,
    _In_ bool DisableVNEClient,
    _In_ bool DisableVNEServer
    )
{
    const uint32_t ServerVersions[] = { QUIC_VERSION_2_H, QUIC_VERSION_1_H };
    const uint32_t ServerVersionsLength = ARRAYSIZE(ServerVersions);
    const uint32_t ExpectedSuccessVersion = QUIC_VERSION_2_H;
    const uint32_t ExpectedFailureVersion = QUIC_VERSION_1_H;

    MsQuicSettings ClientSettings;
    ClientSettings.SetIdleTimeoutMs(3000);

    MsQuicSettings ServerSettings;
    ServerSettings.SetIdleTimeoutMs(3000);

    MsQuicVersionSettings ServerVersionsSettings(ServerVersions, ServerVersionsLength);

    TEST_QUIC_SUCCEEDED(
        MsQuic->SetParam(
            NULL,
            QUIC_PARAM_GLOBAL_VERSION_SETTINGS,
            sizeof(ServerVersionsSettings),
            &ServerVersionsSettings));

    BOOLEAN Value = !DisableVNEServer;
    TEST_QUIC_SUCCEEDED(
        MsQuic->SetParam(
            NULL,
            QUIC_PARAM_GLOBAL_VERSION_NEGOTIATION_ENABLED,
            sizeof(Value),
            &Value));
    ClearGlobalVersionListScope ClearVersionsScope;

    MsQuicRegistration Registration;
    TEST_TRUE(Registration.IsValid());

    MsQuicAlpn Alpn("MsQuicTest");

    MsQuicConfiguration ServerConfiguration(Registration, Alpn, ServerSettings, ServerSelfSignedCredConfig);
    TEST_TRUE(ServerConfiguration.IsValid());

    MsQuicCredentialConfig ClientCredConfig;
    MsQuicConfiguration ClientConfiguration(Registration, Alpn, ClientSettings, ClientCredConfig);
    TEST_TRUE(ClientConfiguration.IsValid());
    TEST_QUIC_SUCCEEDED(ClientConfiguration.SetVersionNegotiationExtEnabled(!DisableVNEClient));

    {
        TestListener Listener(Registration, ListenerAcceptConnection, ServerConfiguration);
        TEST_TRUE(Listener.IsValid());

        QUIC_ADDRESS_FAMILY QuicAddrFamily = (Family == 4) ? QUIC_ADDRESS_FAMILY_INET : QUIC_ADDRESS_FAMILY_INET6;
        QuicAddr ServerLocalAddr(QuicAddrFamily);
        TEST_QUIC_SUCCEEDED(Listener.Start(Alpn, &ServerLocalAddr.SockAddr));
        TEST_QUIC_SUCCEEDED(Listener.GetLocalAddr(ServerLocalAddr));
        {
            UniquePtr<TestConnection> Server;
            ServerAcceptContext ServerAcceptCtx((TestConnection**)&Server);
            Listener.Context = &ServerAcceptCtx;

            {
                TestConnection Client(Registration);
                TEST_TRUE(Client.IsValid());
                Client.SetSslKeyLogFilePath();

                TEST_QUIC_SUCCEEDED(
                    Client.Start(
                        ClientConfiguration,
                        QuicAddrFamily,
                        QUIC_TEST_LOOPBACK_FOR_AF(QuicAddrFamily),
                        ServerLocalAddr.GetPort()));

                if (!Client.WaitForConnectionComplete()) {
                    return;
                }
                TEST_TRUE(Client.GetIsConnected());

                TEST_NOT_EQUAL(nullptr, Server);
                Server->SetSslKeyLogFilePath();
                if (!Server->WaitForConnectionComplete()) {
                    return;
                }
                TEST_TRUE(Server->GetIsConnected());

                if (DisableVNEClient || DisableVNEServer) {
                    TEST_EQUAL(Client.GetQuicVersion(), ExpectedFailureVersion);
                    TEST_EQUAL(Server->GetQuicVersion(), ExpectedFailureVersion);
                } else {
                    TEST_EQUAL(Client.GetQuicVersion(), ExpectedSuccessVersion);
                    TEST_EQUAL(Server->GetQuicVersion(), ExpectedSuccessVersion);
                }
                TEST_FALSE(Client.GetStatistics().VersionNegotiation);
            }
        }
    }
}

void
QuicTestIncompatibleVersionNegotiation(
    _In_ int Family
    )
{
    const uint32_t ClientVersions[] = { QUIC_VERSION_2_H, QUIC_VERSION_1_H };
    const uint32_t ServerVersions[] = { QUIC_VERSION_1_H };
    const uint32_t ClientVersionsLength = ARRAYSIZE(ClientVersions);
    const uint32_t ServerVersionsLength = ARRAYSIZE(ServerVersions);
    const uint32_t ExpectedResultVersion = QUIC_VERSION_1_H;

    MsQuicSettings ClientSettings;
    ClientSettings.SetIdleTimeoutMs(3000);

    MsQuicVersionSettings ClientVersionSettings(ClientVersions, ClientVersionsLength);

    MsQuicSettings ServerSettings;
    ServerSettings.SetIdleTimeoutMs(3000);

    MsQuicVersionSettings ServerVersionsSettings(ServerVersions, ServerVersionsLength);

    TEST_QUIC_SUCCEEDED(
        MsQuic->SetParam(
            NULL,
            QUIC_PARAM_GLOBAL_VERSION_SETTINGS,
            sizeof(ServerVersionsSettings),
            &ServerVersionsSettings));
    ClearGlobalVersionListScope ClearVersionsScope;

    MsQuicRegistration Registration;
    TEST_TRUE(Registration.IsValid());

    MsQuicAlpn Alpn("MsQuicTest");

    MsQuicConfiguration ServerConfiguration(Registration, Alpn, ServerSettings, ServerSelfSignedCredConfig);
    TEST_TRUE(ServerConfiguration.IsValid());

    MsQuicCredentialConfig ClientCredConfig;
    MsQuicConfiguration ClientConfiguration(Registration, Alpn, ClientSettings, ClientCredConfig);
    TEST_TRUE(ClientConfiguration.IsValid());
    TEST_QUIC_SUCCEEDED(ClientConfiguration.SetVersionSettings(ClientVersionSettings));

    {
        TestListener Listener(Registration, ListenerAcceptConnection, ServerConfiguration);
        TEST_TRUE(Listener.IsValid());

        QUIC_ADDRESS_FAMILY QuicAddrFamily = (Family == 4) ? QUIC_ADDRESS_FAMILY_INET : QUIC_ADDRESS_FAMILY_INET6;
        QuicAddr ServerLocalAddr(QuicAddrFamily);
        TEST_QUIC_SUCCEEDED(Listener.Start(Alpn, &ServerLocalAddr.SockAddr));
        TEST_QUIC_SUCCEEDED(Listener.GetLocalAddr(ServerLocalAddr));
        {
            UniquePtr<TestConnection> Server;
            ServerAcceptContext ServerAcceptCtx((TestConnection**)&Server);
            Listener.Context = &ServerAcceptCtx;

            {
                TestConnection Client(Registration);
                TEST_TRUE(Client.IsValid());
                Client.SetSslKeyLogFilePath();

                TEST_QUIC_SUCCEEDED(
                    Client.Start(
                        ClientConfiguration,
                        QuicAddrFamily,
                        QUIC_TEST_LOOPBACK_FOR_AF(QuicAddrFamily),
                        ServerLocalAddr.GetPort()));

                if (!Client.WaitForConnectionComplete()) {
                    return;
                }
                TEST_TRUE(Client.GetIsConnected());

                TEST_NOT_EQUAL(nullptr, Server);
                Server->SetSslKeyLogFilePath();
                if (!Server->WaitForConnectionComplete()) {
                    return;
                }
                TEST_TRUE(Server->GetIsConnected());

                TEST_EQUAL(Client.GetQuicVersion(), ExpectedResultVersion);
                TEST_EQUAL(Server->GetQuicVersion(), ExpectedResultVersion);
                TEST_TRUE(Client.GetStatistics().VersionNegotiation);
            }
        }
    }
}

void
RunFailedVersionNegotiation(
    _In_reads_bytes_(ClientVersionsLength * sizeof(uint32_t))
        const uint32_t* ClientVersions,
    _In_reads_bytes_(ServerVersionsLength * sizeof(uint32_t))
         const uint32_t* ServerVersions,
    _In_ const uint32_t ClientVersionsLength,
    _In_ const uint32_t ServerVersionsLength,
    _In_ QUIC_STATUS ExpectedClientError,
    _In_ QUIC_STATUS ExpectedServerError,
    _In_ uint32_t ExpectedClientVersion,
    _In_ int Family
    )
{
    MsQuicSettings ClientSettings;
    ClientSettings.SetIdleTimeoutMs(2000);
    ClientSettings.SetDisconnectTimeoutMs(1000);

    MsQuicVersionSettings ClientVersionSettings(ClientVersions, ClientVersionsLength);

    MsQuicSettings ServerSettings;
    ServerSettings.SetIdleTimeoutMs(2000);
    ServerSettings.SetDisconnectTimeoutMs(1000);

    MsQuicVersionSettings ServerVersionsSettings(ServerVersions, ServerVersionsLength);

    if (ServerVersions != NULL) {
        TEST_QUIC_SUCCEEDED(
            MsQuic->SetParam(
                NULL,
                QUIC_PARAM_GLOBAL_VERSION_SETTINGS,
                sizeof(ServerVersionsSettings),
                &ServerVersionsSettings));
    } else {
        BOOLEAN Disabled = FALSE;
        TEST_QUIC_SUCCEEDED(
            MsQuic->SetParam(
                NULL,
                QUIC_PARAM_GLOBAL_VERSION_NEGOTIATION_ENABLED,
                sizeof(Disabled),
                &Disabled));
    }
    ClearGlobalVersionListScope ClearVersionsScope;

    MsQuicRegistration Registration;
    TEST_TRUE(Registration.IsValid());

    MsQuicAlpn Alpn("MsQuicTest");

    MsQuicConfiguration ServerConfiguration(Registration, Alpn, ServerSettings, ServerSelfSignedCredConfig);
    TEST_TRUE(ServerConfiguration.IsValid());

    MsQuicCredentialConfig ClientCredConfig;
    MsQuicConfiguration ClientConfiguration(Registration, Alpn, ClientSettings, ClientCredConfig);
    TEST_TRUE(ClientConfiguration.IsValid());
    TEST_QUIC_SUCCEEDED(ClientConfiguration.SetVersionSettings(ClientVersionSettings));

    {
        TestListener Listener(Registration, ListenerAcceptConnection, ServerConfiguration);
        TEST_TRUE(Listener.IsValid());

        QUIC_ADDRESS_FAMILY QuicAddrFamily = (Family == 4) ? QUIC_ADDRESS_FAMILY_INET : QUIC_ADDRESS_FAMILY_INET6;
        QuicAddr ServerLocalAddr(QuicAddrFamily);
        TEST_QUIC_SUCCEEDED(Listener.Start(Alpn, &ServerLocalAddr.SockAddr));
        TEST_QUIC_SUCCEEDED(Listener.GetLocalAddr(ServerLocalAddr));
        {
            UniquePtr<TestConnection> Server;
            ServerAcceptContext ServerAcceptCtx((TestConnection**)&Server);
            Listener.Context = &ServerAcceptCtx;
            if (QUIC_FAILED(ExpectedServerError)) {
                ServerAcceptCtx.ExpectedTransportCloseStatus = ExpectedServerError;
            }

            {
                TestConnection Client(Registration);
                TEST_TRUE(Client.IsValid());
                Client.SetSslKeyLogFilePath();
                Client.SetExpectedTransportCloseStatus(ExpectedClientError);

                TEST_QUIC_SUCCEEDED(
                    Client.Start(
                        ClientConfiguration,
                        QuicAddrFamily,
                        QUIC_TEST_LOOPBACK_FOR_AF(QuicAddrFamily),
                        ServerLocalAddr.GetPort()));

                Client.WaitForShutdownComplete();
                TEST_FALSE(Client.GetIsConnected());

                if (QUIC_FAILED(ExpectedServerError)) {
                    TEST_NOT_EQUAL(nullptr, Server);
                    Server->SetSslKeyLogFilePath();
                } else {
                    TEST_EQUAL(nullptr, Server);
                }

                TEST_EQUAL(Client.GetQuicVersion(), ExpectedClientVersion);
                TEST_TRUE(Client.GetStatistics().VersionNegotiation);
                TEST_EQUAL(Client.GetTransportCloseStatus(), ExpectedClientError);
            }
        }
    }
}

void
QuicTestFailedVersionNegotiation(
    _In_ int Family
    )
{
    const uint32_t NoCommonClientVersions[] = { QUIC_VERSION_DRAFT_29_H };
    const uint32_t NoCommonServerVersions[] = { QUIC_VERSION_1_MS_H };

    RunFailedVersionNegotiation(
        NoCommonClientVersions,
        NoCommonServerVersions,
        ARRAYSIZE(NoCommonClientVersions),
        ARRAYSIZE(NoCommonServerVersions),
        QUIC_STATUS_VER_NEG_ERROR,
        QUIC_STATUS_SUCCESS,
        QUIC_VERSION_DRAFT_29_H,
        Family);

    const uint32_t ClientVersions[] = { 0x0a0a0a0a, QUIC_VERSION_1_H }; // Random reserved version to force VN.

    RunFailedVersionNegotiation(
        ClientVersions,
        NULL,
        ARRAYSIZE(ClientVersions),
        0,
        QUIC_STATUS_VER_NEG_ERROR,
        QUIC_STATUS_VER_NEG_ERROR,
        QUIC_VERSION_1_H,
        Family);
}

void
QuicTestReliableResetNegotiation(
    _In_ int Family,
    _In_ bool ServerSupport,
    _In_ bool ClientSupport
    )
{
    struct Context {
        bool Negotiated {false};
        bool CallbackReceived {false};
        QUIC_STATUS ConnectionCallback(_Inout_ QUIC_CONNECTION_EVENT* Event) {
            if (Event->Type == QUIC_CONNECTION_EVENT_RELIABLE_RESET_NEGOTIATED) {
                CallbackReceived = true;
                Negotiated = Event->RELIABLE_RESET_NEGOTIATED.IsNegotiated;
            }
            return QUIC_STATUS_SUCCESS;
        }
        static QUIC_STATUS s_ConnectionCallback(_In_ MsQuicConnection*, _In_opt_ void* context, _Inout_ QUIC_CONNECTION_EVENT* Event) {
            return ((Context*)context)->ConnectionCallback(Event);
        }
    };

    Context ClientContext, ServerContext;

    MsQuicRegistration Registration(true);
    TEST_TRUE(Registration.IsValid());

    MsQuicSettings ServerSettings;
    MsQuicSettings ClientSettings;
    ServerSettings.SetReliableResetEnabled(ServerSupport);
    ClientSettings.SetReliableResetEnabled(ClientSupport);

    MsQuicConfiguration ServerConfiguration(Registration, "MsQuicTest", ServerSettings, ServerSelfSignedCredConfig);
    TEST_QUIC_SUCCEEDED(ServerConfiguration.GetInitStatus());

    MsQuicConfiguration ClientConfiguration(Registration, "MsQuicTest", ClientSettings, MsQuicCredentialConfig());
    TEST_QUIC_SUCCEEDED(ClientConfiguration.GetInitStatus());

    QUIC_ADDRESS_FAMILY QuicAddrFamily = (Family == 4) ? QUIC_ADDRESS_FAMILY_INET : QUIC_ADDRESS_FAMILY_INET6;
    QuicAddr ServerLocalAddr(QuicAddrFamily);
    MsQuicAutoAcceptListener Listener(Registration, ServerConfiguration, Context::s_ConnectionCallback, &ServerContext);

    TEST_QUIC_SUCCEEDED(Listener.Start("MsQuicTest", &ServerLocalAddr.SockAddr));
    TEST_QUIC_SUCCEEDED(Listener.GetInitStatus());
    TEST_QUIC_SUCCEEDED(Listener.GetLocalAddr(ServerLocalAddr));

    MsQuicConnection Connection(Registration, CleanUpManual, Context::s_ConnectionCallback, &ClientContext);
    TEST_QUIC_SUCCEEDED(Connection.GetInitStatus());
    TEST_QUIC_SUCCEEDED(Connection.Start(ClientConfiguration, ServerLocalAddr.GetFamily(), QUIC_TEST_LOOPBACK_FOR_AF(ServerLocalAddr.GetFamily()), ServerLocalAddr.GetPort()));

    TEST_TRUE(Connection.HandshakeCompleteEvent.WaitTimeout(TestWaitTimeout));
    TEST_TRUE(Connection.HandshakeComplete);
    TEST_TRUE(Listener.LastConnection->HandshakeCompleteEvent.WaitTimeout(TestWaitTimeout));
    TEST_TRUE(Listener.LastConnection->HandshakeComplete);

    MsQuicSettings ListenerServerSettings2;
    TEST_QUIC_SUCCEEDED(Listener.LastConnection->GetSettings(&ListenerServerSettings2));
    TEST_EQUAL(ListenerServerSettings2.ReliableResetEnabled, (int) ServerSupport);

    if (ClientSupport) {
        TEST_TRUE(ClientContext.CallbackReceived);
        TEST_TRUE(ClientContext.Negotiated == ServerSupport);
    } else {
        TEST_FALSE(ClientContext.CallbackReceived);
    }
    if (ServerSupport) {
        TEST_TRUE(ServerContext.CallbackReceived);
        TEST_TRUE(ServerContext.Negotiated == ClientSupport);
    } else {
        TEST_FALSE(ServerContext.CallbackReceived);
    }
}

void
QuicTestOneWayDelayNegotiation(
    _In_ int Family,
    _In_ bool ServerSupport,
    _In_ bool ClientSupport
    )
{
    struct Context {
        bool SendNegotiated {false};
        bool RecvNegotiated {false};
        bool CallbackReceived {false};
        QUIC_STATUS ConnectionCallback(_Inout_ QUIC_CONNECTION_EVENT* Event) {
            if (Event->Type == QUIC_CONNECTION_EVENT_ONE_WAY_DELAY_NEGOTIATED) {
                CallbackReceived = true;
                SendNegotiated = Event->ONE_WAY_DELAY_NEGOTIATED.SendNegotiated;
                RecvNegotiated = Event->ONE_WAY_DELAY_NEGOTIATED.ReceiveNegotiated;
            }
            return QUIC_STATUS_SUCCESS;
        }
        static QUIC_STATUS s_ConnectionCallback(_In_ MsQuicConnection*, _In_opt_ void* context, _Inout_ QUIC_CONNECTION_EVENT* Event) {
            return ((Context*)context)->ConnectionCallback(Event);
        }
    } ClientContext, ServerContext;

    MsQuicRegistration Registration(true);
    TEST_TRUE(Registration.IsValid());

    MsQuicSettings ServerSettings; ServerSettings.SetOneWayDelayEnabled(ServerSupport);
    MsQuicConfiguration ServerConfiguration(Registration, "MsQuicTest", ServerSettings, ServerSelfSignedCredConfig);
    TEST_QUIC_SUCCEEDED(ServerConfiguration.GetInitStatus());

    MsQuicSettings ClientSettings; ClientSettings.SetOneWayDelayEnabled(ClientSupport);
    MsQuicConfiguration ClientConfiguration(Registration, "MsQuicTest", ClientSettings, MsQuicCredentialConfig());
    TEST_QUIC_SUCCEEDED(ClientConfiguration.GetInitStatus());

    QUIC_ADDRESS_FAMILY QuicAddrFamily = (Family == 4) ? QUIC_ADDRESS_FAMILY_INET : QUIC_ADDRESS_FAMILY_INET6;
    QuicAddr ServerLocalAddr(QuicAddrFamily);
    MsQuicAutoAcceptListener Listener(Registration, ServerConfiguration, Context::s_ConnectionCallback, &ServerContext);
    TEST_QUIC_SUCCEEDED(Listener.Start("MsQuicTest", &ServerLocalAddr.SockAddr));
    TEST_QUIC_SUCCEEDED(Listener.GetInitStatus());
    TEST_QUIC_SUCCEEDED(Listener.GetLocalAddr(ServerLocalAddr));

    MsQuicConnection Connection(Registration, CleanUpManual, Context::s_ConnectionCallback, &ClientContext);
    TEST_QUIC_SUCCEEDED(Connection.GetInitStatus());
    TEST_QUIC_SUCCEEDED(Connection.Start(ClientConfiguration, ServerLocalAddr.GetFamily(), QUIC_TEST_LOOPBACK_FOR_AF(ServerLocalAddr.GetFamily()), ServerLocalAddr.GetPort()));
    TEST_TRUE(Connection.HandshakeCompleteEvent.WaitTimeout(TestWaitTimeout));
    TEST_TRUE(Connection.HandshakeComplete);
    TEST_TRUE(Listener.LastConnection->HandshakeCompleteEvent.WaitTimeout(TestWaitTimeout));
    TEST_TRUE(Listener.LastConnection->HandshakeComplete);

    MsQuicSettings ListenerServerSettings2;
    TEST_QUIC_SUCCEEDED(Listener.LastConnection->GetSettings(&ListenerServerSettings2));
    TEST_EQUAL(ListenerServerSettings2.OneWayDelayEnabled, (int)ServerSupport);

    if (ClientSupport) {
        TEST_TRUE(ClientContext.CallbackReceived);
        TEST_TRUE(ClientContext.SendNegotiated == ServerSupport);
        TEST_TRUE(ClientContext.RecvNegotiated == ServerSupport);
    } else {
        TEST_FALSE(ClientContext.CallbackReceived);
    }
    if (ServerSupport) {
        TEST_TRUE(ServerContext.CallbackReceived);
        TEST_TRUE(ServerContext.SendNegotiated == ClientSupport);
        TEST_TRUE(ServerContext.RecvNegotiated == ClientSupport);
    } else {
        TEST_FALSE(ServerContext.CallbackReceived);
    }
}

#endif // QUIC_API_ENABLE_PREVIEW_FEATURES

void
QuicTestConnectBadAlpn(
    _In_ int Family
    )
{
    MsQuicRegistration Registration;
    TEST_TRUE(Registration.IsValid());

    MsQuicAlpn Alpn("MsQuicTest");

    MsQuicSettings Settings;
    Settings.SetIdleTimeoutMs(3000);

    MsQuicConfiguration ServerConfiguration(Registration, Alpn, Settings, ServerSelfSignedCredConfig);
    TEST_TRUE(ServerConfiguration.IsValid());

    MsQuicCredentialConfig ClientCredConfig;
    MsQuicConfiguration ClientConfiguration(Registration, "BadALPN", Settings, ClientCredConfig);
    TEST_TRUE(ClientConfiguration.IsValid());

    {
        TestListener Listener(Registration, ListenerAcceptConnection, ServerConfiguration);
        TEST_TRUE(Listener.IsValid());
        TEST_QUIC_SUCCEEDED(Listener.Start(Alpn));

        QUIC_ADDRESS_FAMILY QuicAddrFamily = (Family == 4) ? QUIC_ADDRESS_FAMILY_INET : QUIC_ADDRESS_FAMILY_INET6;
        QuicAddr ServerLocalAddr;
        TEST_QUIC_SUCCEEDED(Listener.GetLocalAddr(ServerLocalAddr));

        {
            UniquePtr<TestConnection> Server;
            ServerAcceptContext ServerAcceptCtx((TestConnection**)&Server);
            Listener.Context = &ServerAcceptCtx;

            {
                TestConnection Client(Registration);
                TEST_TRUE(Client.IsValid());
                Client.SetSslKeyLogFilePath();

                Client.SetExpectedTransportCloseStatus(QUIC_STATUS_ALPN_NEG_FAILURE);
                TEST_QUIC_SUCCEEDED(
                    Client.Start(
                        ClientConfiguration,
                        QuicAddrFamily,
                        QUIC_TEST_LOOPBACK_FOR_AF(QuicAddrFamily),
                        ServerLocalAddr.GetPort()));
                if (!Client.WaitForConnectionComplete()) {
                    return;
                }

                TEST_FALSE(Client.GetIsConnected());
                TEST_TRUE(Client.GetTransportClosed());

                TEST_EQUAL(nullptr, Server);
            }
        }
    }
}

void
QuicTestConnectBadSni(
    _In_ int Family
    )
{
    MsQuicRegistration Registration;
    TEST_TRUE(Registration.IsValid());

    MsQuicAlpn Alpn("MsQuicTest");

    MsQuicSettings Settings;
    Settings.SetIdleTimeoutMs(3000);

    MsQuicConfiguration ServerConfiguration(Registration, Alpn, Settings, ServerSelfSignedCredConfig);
    TEST_TRUE(ServerConfiguration.IsValid());

    MsQuicCredentialConfig ClientCredConfig;
    MsQuicConfiguration ClientConfiguration(Registration, Alpn, Settings, ClientCredConfig);
    TEST_TRUE(ClientConfiguration.IsValid());

    {
        TestListener Listener(Registration, ListenerAcceptConnection, ServerConfiguration);
        TEST_TRUE(Listener.IsValid());
        TEST_QUIC_SUCCEEDED(Listener.Start(Alpn));

        QuicAddr ServerLocalAddr;
        TEST_QUIC_SUCCEEDED(Listener.GetLocalAddr(ServerLocalAddr));

        {
            UniquePtr<TestConnection> Server;
            ServerAcceptContext ServerAcceptCtx((TestConnection**)&Server);
            Listener.Context = &ServerAcceptCtx;

            {
                TestConnection Client(Registration);
                TEST_TRUE(Client.IsValid());
                Client.SetSslKeyLogFilePath();

                QuicAddr RemoteAddr(Family == 4 ? QUIC_ADDRESS_FAMILY_INET : QUIC_ADDRESS_FAMILY_INET6, true);
                if (UseDuoNic) {
                    QuicAddrSetToDuoNic(&RemoteAddr.SockAddr);
                }
                TEST_QUIC_SUCCEEDED(Client.SetRemoteAddr(RemoteAddr));

                Client.SetExpectedTransportCloseStatus(QUIC_STATUS_CONNECTION_REFUSED);
                TEST_QUIC_SUCCEEDED(
                    Client.Start(
                        ClientConfiguration,
                        Family == 4 ? QUIC_ADDRESS_FAMILY_INET : QUIC_ADDRESS_FAMILY_INET6,
                        "badlocalhost",
                        ServerLocalAddr.GetPort()));
                if (!Client.WaitForConnectionComplete()) {
                    return;
                }

                TEST_FALSE(Client.GetIsConnected());
                TEST_TRUE(Client.GetTransportClosed());

                TEST_EQUAL(nullptr, Server);
            }
        }
    }
}

_Function_class_(NEW_CONNECTION_CALLBACK)
static
bool
QUIC_API
ListenerRejectConnection(
    _In_ TestListener* /*  Listener */,
    _In_ HQUIC ConnectionHandle
    )
{
    auto Connection = new(std::nothrow) TestConnection(ConnectionHandle);
    if (Connection == nullptr || !Connection->IsValid()) {
        TEST_FAILURE("Failed to accept new TestConnection.");
        delete Connection;
        return false;
    }
    Connection->SetAutoDelete();
    Connection->Shutdown(QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, QUIC_TEST_SPECIAL_ERROR);
    return true;
}

void
QuicTestConnectServerRejected(
    _In_ int Family
    )
{
    MsQuicRegistration Registration;
    TEST_TRUE(Registration.IsValid());

    MsQuicAlpn Alpn("MsQuicTest");

    MsQuicSettings Settings;
    Settings.SetIdleTimeoutMs(3000);
    Settings.SetSendBufferingEnabled(true);

    MsQuicConfiguration ServerConfiguration(Registration, Alpn, Settings, ServerSelfSignedCredConfig);
    TEST_TRUE(ServerConfiguration.IsValid());

    MsQuicCredentialConfig ClientCredConfig;
    MsQuicConfiguration ClientConfiguration(Registration, Alpn, Settings, ClientCredConfig);
    TEST_TRUE(ClientConfiguration.IsValid());

    {
        TestListener Listener(Registration, ListenerRejectConnection, ServerConfiguration);
        TEST_TRUE(Listener.IsValid());
        TEST_QUIC_SUCCEEDED(Listener.Start(Alpn));

        QUIC_ADDRESS_FAMILY QuicAddrFamily = (Family == 4) ? QUIC_ADDRESS_FAMILY_INET : QUIC_ADDRESS_FAMILY_INET6;
        QuicAddr ServerLocalAddr;
        TEST_QUIC_SUCCEEDED(Listener.GetLocalAddr(ServerLocalAddr));

        {
            TestConnection Client(Registration);
            TEST_TRUE(Client.IsValid());

            Client.SetExpectedTransportCloseStatus(QUIC_STATUS_USER_CANCELED);
            TEST_QUIC_SUCCEEDED(
                Client.Start(
                    ClientConfiguration,
                    QuicAddrFamily,
                    QUIC_TEST_LOOPBACK_FOR_AF(QuicAddrFamily),
                    ServerLocalAddr.GetPort()));
            if (!Client.WaitForShutdownComplete()) {
                return;
            }
        }
    }
}

void
QuicTestKeyUpdateRandomLoss(
    _In_ int Family,
    _In_ uint8_t RandomLossPercentage
    )
{
    MsQuicRegistration Registration;
    TEST_TRUE(Registration.IsValid());

    MsQuicAlpn Alpn("MsQuicTest");

    MsQuicSettings Settings;
    MsQuicConfiguration ServerConfiguration(Registration, Alpn, Settings, ServerSelfSignedCredConfig);
    TEST_TRUE(ServerConfiguration.IsValid());

    MsQuicCredentialConfig ClientCredConfig;
    MsQuicConfiguration ClientConfiguration(Registration, Alpn, Settings, ClientCredConfig);
    TEST_TRUE(ClientConfiguration.IsValid());

    const int Iterations = 10;

    {
        TestListener Listener(Registration, ListenerAcceptConnection, ServerConfiguration);
        TEST_TRUE(Listener.IsValid());
        Listener.SetHasRandomLoss(true);

        QUIC_ADDRESS_FAMILY QuicAddrFamily = (Family == 4) ? QUIC_ADDRESS_FAMILY_INET : QUIC_ADDRESS_FAMILY_INET6;
        QuicAddr ServerLocalAddr(QuicAddrFamily);
        TEST_QUIC_SUCCEEDED(Listener.Start(Alpn, &ServerLocalAddr.SockAddr));
        TEST_QUIC_SUCCEEDED(Listener.GetLocalAddr(ServerLocalAddr));

        {
            UniquePtr<TestConnection> Server;
            ServerAcceptContext ServerAcceptCtx((TestConnection**)&Server);
            Listener.Context = &ServerAcceptCtx;

            {
                TestConnection Client(Registration);
                TEST_TRUE(Client.IsValid());
                Client.SetHasRandomLoss(true);

                TEST_QUIC_SUCCEEDED(
                    Client.Start(
                        ClientConfiguration,
                        QuicAddrFamily,
                        QUIC_TEST_LOOPBACK_FOR_AF(QuicAddrFamily),
                        ServerLocalAddr.GetPort()));

                if (!Client.WaitForConnectionComplete()) {
                    return;
                }
                TEST_TRUE(Client.GetIsConnected());

                TEST_NOT_EQUAL(nullptr, Server);
                if (!Server->WaitForConnectionComplete()) {
                    return;
                }
                TEST_TRUE(Server->GetIsConnected());

                {
                    RandomLossHelper LossHelper(RandomLossPercentage);

                    CxPlatSleep(100);

                    for (uint16_t i = 0; i < Iterations; ++i) {

                        //
                        // We don't care if this call succeeds, we just want to trigger it every time
                        //
                        Client.ForceKeyUpdate();
                        Server->ForceKeyUpdate();

                        //
                        // Send some data to perform the key update.
                        // TODO: Update this to send stream data, like QuicConnectAndPing does.
                        //
                        TEST_QUIC_SUCCEEDED(Client.SetPeerBidiStreamCount((uint16_t)(101 + i)));
                        TEST_EQUAL((uint16_t)(101 + i), Client.GetPeerBidiStreamCount());
                        CxPlatSleep(50);

                        //
                        // Force a client key update to occur again to check for double update
                        // while server is still waiting for key response.
                        //
                        Client.ForceKeyUpdate();

                        TEST_QUIC_SUCCEEDED(Server->SetPeerBidiStreamCount((uint16_t)(100 + i)));
                        TEST_EQUAL((uint16_t)(100 + i), Server->GetPeerBidiStreamCount());
                        CxPlatSleep(50);
                    }

                    CxPlatSleep(100);
                }

                QUIC_STATISTICS_V2 Stats = Client.GetStatistics();
                if (Stats.RecvDecryptionFailures) {
                    TEST_FAILURE("%llu server packets failed to decrypt!", Stats.RecvDecryptionFailures);
                    return;
                }

                if (Stats.KeyUpdateCount < 1) {
                    TEST_FAILURE("%u Key updates occured. Expected at least 1", Stats.KeyUpdateCount);
                    return;
                }

                Stats = Server->GetStatistics();
                if (Stats.RecvDecryptionFailures) {
                    TEST_FAILURE("%llu client packets failed to decrypt!", Stats.RecvDecryptionFailures);
                    return;
                }

                if (Stats.KeyUpdateCount < 1) {
                    TEST_FAILURE("%u Key updates occured. Expected at least 1", Stats.KeyUpdateCount);
                    return;
                }

                Client.Shutdown(QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, QUIC_TEST_NO_ERROR);
                if (!Client.WaitForShutdownComplete()) {
                    return;
                }
            }
        }
    }
}

void
QuicTestKeyUpdate(
    _In_ int Family,
    _In_ uint16_t Iterations,
    _In_ uint16_t KeyUpdateBytes,
    _In_ bool UseKeyUpdateBytes,
    _In_ bool ClientKeyUpdate,
    _In_ bool ServerKeyUpdate
    )
{
    MsQuicRegistration Registration;
    TEST_TRUE(Registration.IsValid());

    MsQuicAlpn Alpn("MsQuicTest");

    MsQuicSettings Settings;
    if (UseKeyUpdateBytes) {
        Settings.SetMaxBytesPerKey(KeyUpdateBytes);
    }

    MsQuicConfiguration ServerConfiguration(Registration, Alpn, Settings, ServerSelfSignedCredConfig);
    TEST_TRUE(ServerConfiguration.IsValid());

    MsQuicCredentialConfig ClientCredConfig;
    MsQuicConfiguration ClientConfiguration(Registration, Alpn, Settings, ClientCredConfig);
    TEST_TRUE(ClientConfiguration.IsValid());

    {
        TestListener Listener(Registration, ListenerAcceptConnection, ServerConfiguration);
        TEST_TRUE(Listener.IsValid());

        QUIC_ADDRESS_FAMILY QuicAddrFamily = (Family == 4) ? QUIC_ADDRESS_FAMILY_INET : QUIC_ADDRESS_FAMILY_INET6;
        QuicAddr ServerLocalAddr(QuicAddrFamily);
        TEST_QUIC_SUCCEEDED(Listener.Start(Alpn, &ServerLocalAddr.SockAddr));
        TEST_QUIC_SUCCEEDED(Listener.GetLocalAddr(ServerLocalAddr));

        {
            UniquePtr<TestConnection> Server;
            ServerAcceptContext ServerAcceptCtx((TestConnection**)&Server);
            Listener.Context = &ServerAcceptCtx;

            {
                TestConnection Client(Registration);
                TEST_TRUE(Client.IsValid());

                TEST_QUIC_SUCCEEDED(
                    Client.Start(
                        ClientConfiguration,
                        QuicAddrFamily,
                        QUIC_TEST_LOOPBACK_FOR_AF(QuicAddrFamily),
                        ServerLocalAddr.GetPort()));

                if (!Client.WaitForConnectionComplete()) {
                    return;
                }
                TEST_TRUE(Client.GetIsConnected());

                TEST_NOT_EQUAL(nullptr, Server);
                if (!Server->WaitForConnectionComplete()) {
                    return;
                }
                TEST_TRUE(Server->GetIsConnected());

                for (uint16_t i = 0; i < Iterations; ++i) {

                    CxPlatSleep(100);

                    if (ClientKeyUpdate) {
                        TEST_QUIC_SUCCEEDED(Client.ForceKeyUpdate());
                    }

                    if (ServerKeyUpdate) {
                        TEST_QUIC_SUCCEEDED(Server->ForceKeyUpdate());
                    }

                    //
                    // Send some data to perform the key update.
                    // TODO: Update this to send stream data, like QuicConnectAndPing does.
                    //
                    uint16_t PeerCount, Expected = 101+i, Tries = 0;
                    TEST_QUIC_SUCCEEDED(Client.SetPeerBidiStreamCount(Expected));
                    TEST_EQUAL(Expected, Client.GetPeerBidiStreamCount());

                    do {
                        CxPlatSleep(100);
                        PeerCount =  Server->GetLocalBidiStreamCount();
                    } while (PeerCount != Expected && Tries++ < 10);
                    TEST_EQUAL(Expected, PeerCount);

                    //
                    // Force a client key update to occur again to check for double update
                    // while server is still waiting for key response.
                    //
                    if (ClientKeyUpdate) {
                        TEST_QUIC_SUCCEEDED(Client.ForceKeyUpdate());
                    }

                    Expected = 100+i;
                    TEST_QUIC_SUCCEEDED(Server->SetPeerBidiStreamCount(Expected));
                    TEST_EQUAL(Expected, Server->GetPeerBidiStreamCount());

                    Tries = 0;
                    do {
                        CxPlatSleep(100);
                        PeerCount =  Client.GetLocalBidiStreamCount();
                    } while (PeerCount != Expected && Tries++ < 10);
                    TEST_EQUAL(Expected, PeerCount);
                }

                CxPlatSleep(100);

                QUIC_STATISTICS_V2 Stats = Client.GetStatistics();
                if (Stats.RecvDecryptionFailures) {
                    TEST_FAILURE("%llu server packets failed to decrypt!", Stats.RecvDecryptionFailures);
                    return;
                }

                uint16_t ExpectedUpdates = Iterations - (UseKeyUpdateBytes ? 1u : 0u);

                if (Stats.KeyUpdateCount < ExpectedUpdates) {
                    TEST_FAILURE("%u Key updates occured. Expected %d", Stats.KeyUpdateCount, ExpectedUpdates);
                    return;
                }

                Stats = Server->GetStatistics();
                if (Stats.RecvDecryptionFailures) {
                    TEST_FAILURE("%llu client packets failed to decrypt!", Stats.RecvDecryptionFailures);
                    return;
                }

                if (Stats.KeyUpdateCount < ExpectedUpdates) {
                    TEST_FAILURE("%u Key updates occured. Expected %d", Stats.KeyUpdateCount, ExpectedUpdates);
                    return;
                }

                Client.Shutdown(QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, QUIC_TEST_NO_ERROR);
                if (!Client.WaitForShutdownComplete()) {
                    return;
                }

                TEST_FALSE(Client.GetPeerClosed());
                TEST_FALSE(Client.GetTransportClosed());
            }

#if !QUIC_SEND_FAKE_LOSS
            TEST_TRUE(Server->GetPeerClosed());
            TEST_EQUAL(Server->GetPeerCloseErrorCode(), QUIC_TEST_NO_ERROR);
#endif
        }
    }
}

void
QuicTestCidUpdate(
    _In_ int Family,
    _In_ uint16_t Iterations
    )
{
    MsQuicRegistration Registration;
    TEST_TRUE(Registration.IsValid());

    MsQuicAlpn Alpn("MsQuicTest");

    MsQuicConfiguration ServerConfiguration(Registration, Alpn, ServerSelfSignedCredConfig);
    TEST_TRUE(ServerConfiguration.IsValid());

    MsQuicCredentialConfig ClientCredConfig;
    MsQuicConfiguration ClientConfiguration(Registration, Alpn, ClientCredConfig);
    TEST_TRUE(ClientConfiguration.IsValid());

    {
        TestListener Listener(Registration, ListenerAcceptConnection, ServerConfiguration);
        TEST_TRUE(Listener.IsValid());

        QUIC_ADDRESS_FAMILY QuicAddrFamily = (Family == 4) ? QUIC_ADDRESS_FAMILY_INET : QUIC_ADDRESS_FAMILY_INET6;
        QuicAddr ServerLocalAddr(QuicAddrFamily);
        TEST_QUIC_SUCCEEDED(Listener.Start(Alpn, &ServerLocalAddr.SockAddr));
        TEST_QUIC_SUCCEEDED(Listener.GetLocalAddr(ServerLocalAddr));

        {
            UniquePtr<TestConnection> Server;
            ServerAcceptContext ServerAcceptCtx((TestConnection**)&Server);
            Listener.Context = &ServerAcceptCtx;

            {
                TestConnection Client(Registration);
                TEST_TRUE(Client.IsValid());

                TEST_QUIC_SUCCEEDED(
                    Client.Start(
                        ClientConfiguration,
                        QuicAddrFamily,
                        QUIC_TEST_LOOPBACK_FOR_AF(QuicAddrFamily),
                        ServerLocalAddr.GetPort()));

                if (!Client.WaitForConnectionComplete()) {
                    return;
                }
                TEST_TRUE(Client.GetIsConnected());

                TEST_NOT_EQUAL(nullptr, Server);
                if (!Server->WaitForConnectionComplete()) {
                    return;
                }
                TEST_TRUE(Server->GetIsConnected());

                for (uint16_t i = 0; i < Iterations; ++i) {

                    CxPlatSleep(100);

                    TEST_QUIC_SUCCEEDED(Client.ForceCidUpdate());

                    TEST_QUIC_SUCCEEDED(Client.SetPeerBidiStreamCount((uint16_t)(101+i)));
                    TEST_EQUAL((uint16_t)(101+i), Client.GetPeerBidiStreamCount());
                    CxPlatSleep(100);
                    TEST_EQUAL((uint16_t)(101+i), Server->GetLocalBidiStreamCount());

                    TEST_QUIC_SUCCEEDED(Server->SetPeerBidiStreamCount((uint16_t)(100+i)));
                    TEST_EQUAL((uint16_t)(100+i), Server->GetPeerBidiStreamCount());
                    CxPlatSleep(100);
                    TEST_EQUAL((uint16_t)(100+i), Client.GetLocalBidiStreamCount());
                }

                CxPlatSleep(100);

                QUIC_STATISTICS_V2 Stats = Client.GetStatistics();
                if (Stats.RecvDecryptionFailures) {
                    TEST_FAILURE("%llu server packets failed to decrypt!", Stats.RecvDecryptionFailures);
                    return;
                }

                Stats = Server->GetStatistics();
                if (Stats.RecvDecryptionFailures) {
                    TEST_FAILURE("%llu client packets failed to decrypt!", Stats.RecvDecryptionFailures);
                    return;
                }

                Client.Shutdown(QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, QUIC_TEST_NO_ERROR);
                if (!Client.WaitForShutdownComplete()) {
                    return;
                }

                TEST_FALSE(Client.GetPeerClosed());
                TEST_FALSE(Client.GetTransportClosed());
            }

#if !QUIC_SEND_FAKE_LOSS
            TEST_TRUE(Server->GetPeerClosed());
            TEST_EQUAL(Server->GetPeerCloseErrorCode(), QUIC_TEST_NO_ERROR);
#endif
        }
    }
}

void
QuicTestConnectClientCertificate(
    _In_ int Family,
    _In_ bool UseClientCertificate
    )
{
    MsQuicRegistration Registration;
    TEST_TRUE(Registration.IsValid());

    MsQuicAlpn Alpn("MsQuicTest");

    MsQuicSettings Settings;
    Settings.SetIdleTimeoutMs(3000);

    MsQuicConfiguration ServerConfiguration(Registration, Alpn, Settings, ServerSelfSignedCredConfigClientAuth);
    TEST_TRUE(ServerConfiguration.IsValid());

    MsQuicCredentialConfig ClientNoCertCredConfig;
    MsQuicConfiguration ClientConfiguration(Registration, Alpn, Settings, UseClientCertificate ? ClientCertCredConfig : ClientNoCertCredConfig);
    TEST_TRUE(ClientConfiguration.IsValid());

    QUIC_ADDRESS_FAMILY QuicAddrFamily = (Family == 4) ? QUIC_ADDRESS_FAMILY_INET : QUIC_ADDRESS_FAMILY_INET6;

    {
        TestListener Listener(Registration, ListenerAcceptConnection, ServerConfiguration);
        TEST_TRUE(Listener.IsValid());
        QuicAddr ServerLocalAddr(QuicAddrFamily);
        TEST_QUIC_SUCCEEDED(Listener.Start(Alpn, &ServerLocalAddr.SockAddr));

        TEST_QUIC_SUCCEEDED(Listener.GetLocalAddr(ServerLocalAddr));

        {
            UniquePtr<TestConnection> Server;
            ServerAcceptContext ServerAcceptCtx((TestConnection**)&Server);
            ServerAcceptCtx.AddExpectedClientCertValidationResult(QUIC_STATUS_CERT_UNTRUSTED_ROOT);
            if (!UseClientCertificate) {
                ServerAcceptCtx.AddExpectedClientCertValidationResult(QUIC_STATUS_CERT_NO_CERT);
                ServerAcceptCtx.PeerCertEventReturnStatus = QUIC_STATUS_CONNECTION_REFUSED;
                ServerAcceptCtx.ExpectedTransportCloseStatus = QUIC_STATUS_REQUIRED_CERTIFICATE;
            }
            Listener.Context = &ServerAcceptCtx;

            {
                TestConnection Client(Registration);
                TEST_TRUE(Client.IsValid());
                Client.SetSslKeyLogFilePath();
                if (!UseClientCertificate) {
                    Client.SetExpectedTransportCloseStatus(QUIC_STATUS_REQUIRED_CERTIFICATE);
                }

                TEST_QUIC_SUCCEEDED(
                    Client.Start(
                        ClientConfiguration,
                        QuicAddrFamily,
                        QUIC_TEST_LOOPBACK_FOR_AF(
                            QuicAddrGetFamily(&ServerLocalAddr.SockAddr)),
                        ServerLocalAddr.GetPort()));

                if (!Client.WaitForConnectionComplete()) {
                    return;
                }

                TEST_NOT_EQUAL(nullptr, Server);
                Server->SetSslKeyLogFilePath();
                if (UseClientCertificate) {
                    if (!Server->WaitForConnectionComplete()) {
                        return;
                    }
                }
                TEST_EQUAL(UseClientCertificate, Server->GetIsConnected());
            }
        }
    }
}

void
QuicTestInvalidAlpnLengths(
    void
    )
{
    MsQuicRegistration Registration;
    TEST_TRUE(Registration.IsValid());

    int Lengths[] = { 0, QUIC_MAX_ALPN_LENGTH + 1 };
    char AlpnBuffer[QUIC_MAX_ALPN_LENGTH + 3]; // + 3 so it can always be 0 terminated
    for (int Len = 0; Len < (int)ARRAYSIZE(Lengths); Len++) {
        int AlpnLength = Lengths[Len];
        CxPlatZeroMemory(AlpnBuffer, sizeof(AlpnBuffer));
        for (int i = 0; i < AlpnLength; i++) {
            AlpnBuffer[i] = 'a';
        }

        MsQuicAlpn Alpn(AlpnBuffer);

        MsQuicSettings Settings;
        Settings.SetIdleTimeoutMs(3000);

        MsQuicConfiguration ServerConfiguration(Registration, Alpn, Settings, ServerSelfSignedCredConfig);
        TEST_FALSE(ServerConfiguration.IsValid());
    }
}

void
QuicTestValidAlpnLengths(
    void
    )
{
    MsQuicRegistration Registration;
    TEST_TRUE(Registration.IsValid());

    char AlpnBuffer[QUIC_MAX_ALPN_LENGTH + 2]; // + 2 so it can always be 0 terminated
    for (int AlpnLength = 1; AlpnLength <= QUIC_MAX_ALPN_LENGTH; AlpnLength++) {
        CxPlatZeroMemory(AlpnBuffer, sizeof(AlpnBuffer));
        for (int i = 0; i < AlpnLength; i++) {
            AlpnBuffer[i] = 'a';
        }

        MsQuicAlpn Alpn(AlpnBuffer);

        MsQuicSettings Settings;
        Settings.SetIdleTimeoutMs(3000);

        MsQuicConfiguration ServerConfiguration(Registration, Alpn, Settings, ServerSelfSignedCredConfig);
        TEST_TRUE(ServerConfiguration.IsValid());

        MsQuicCredentialConfig ClientCredConfig;
        MsQuicConfiguration ClientConfiguration(Registration, Alpn, Settings, ClientCredConfig);
        TEST_TRUE(ClientConfiguration.IsValid());

        QUIC_ADDRESS_FAMILY QuicAddrFamily = QUIC_ADDRESS_FAMILY_INET;

        {
            TestListener Listener(Registration, ListenerAcceptConnection, ServerConfiguration);
            TEST_TRUE(Listener.IsValid());
            QuicAddr ServerLocalAddr(QuicAddrFamily);
            TEST_QUIC_SUCCEEDED(Listener.Start(Alpn, &ServerLocalAddr.SockAddr));

            TEST_QUIC_SUCCEEDED(Listener.GetLocalAddr(ServerLocalAddr));

            {
                UniquePtr<TestConnection> Server;
                ServerAcceptContext ServerAcceptCtx((TestConnection**)&Server);
                Listener.Context = &ServerAcceptCtx;

                {
                    TestConnection Client(Registration);
                    TEST_TRUE(Client.IsValid());
                    Client.SetSslKeyLogFilePath();

                    TEST_QUIC_SUCCEEDED(
                        Client.Start(
                            ClientConfiguration,
                            QuicAddrFamily,
                            QUIC_TEST_LOOPBACK_FOR_AF(
                                QuicAddrGetFamily(&ServerLocalAddr.SockAddr)),
                            ServerLocalAddr.GetPort()));

                    if (!Client.WaitForConnectionComplete()) {
                        return;
                    }
                    TEST_TRUE(Client.GetIsConnected());

                    TEST_NOT_EQUAL(nullptr, Server);
                    Server->SetSslKeyLogFilePath();
                    if (!Server->WaitForConnectionComplete()) {
                        return;
                    }
                    TEST_TRUE(Server->GetIsConnected());
                }
            }
        }
    }
}

void
QuicTestConnectExpiredServerCertificate(
    _In_ const QUIC_CREDENTIAL_CONFIG* Config
    )
{
    MsQuicRegistration Registration;
    TEST_TRUE(Registration.IsValid());

    MsQuicAlpn Alpn("MsQuicTest");

    MsQuicSettings Settings;
    Settings.SetIdleTimeoutMs(3000);

    MsQuicConfiguration ServerConfiguration(Registration, Alpn, Settings, *Config);
    TEST_TRUE(ServerConfiguration.IsValid());

    MsQuicCredentialConfig ClientCredConfig;
    ClientCredConfig.Flags &= ~QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
    MsQuicConfiguration ClientConfiguration(Registration, Alpn, Settings, ClientCredConfig);
    TEST_TRUE(ClientConfiguration.IsValid());

    QUIC_ADDRESS_FAMILY QuicAddrFamily = QUIC_ADDRESS_FAMILY_INET;

    {
        TestListener Listener(Registration, ListenerAcceptConnection, ServerConfiguration);
        TEST_TRUE(Listener.IsValid());
        QuicAddr ServerLocalAddr(QuicAddrFamily);
        TEST_QUIC_SUCCEEDED(Listener.Start(Alpn, &ServerLocalAddr.SockAddr));

        TEST_QUIC_SUCCEEDED(Listener.GetLocalAddr(ServerLocalAddr));

        {
            UniquePtr<TestConnection> Server;
            ServerAcceptContext ServerAcceptCtx((TestConnection**)&Server);
            ServerAcceptCtx.ExpectedTransportCloseStatus = QUIC_STATUS_EXPIRED_CERTIFICATE;
            Listener.Context = &ServerAcceptCtx;

            {
                TestConnection Client(Registration);
                TEST_TRUE(Client.IsValid());
                Client.SetSslKeyLogFilePath();
                Client.SetExpectedTransportCloseStatus(QUIC_STATUS_EXPIRED_CERTIFICATE);

                TEST_QUIC_SUCCEEDED(
                    Client.Start(
                        ClientConfiguration,
                        QuicAddrFamily,
                        QUIC_TEST_LOOPBACK_FOR_AF(
                            QuicAddrGetFamily(&ServerLocalAddr.SockAddr)),
                        ServerLocalAddr.GetPort()));

                if (!Client.WaitForConnectionComplete()) {
                    return;
                }
                TEST_EQUAL(false, Client.GetIsConnected());

                TEST_NOT_EQUAL(nullptr, Server);
                Server->SetSslKeyLogFilePath();
                if (!Server->WaitForConnectionComplete()) {
                    return;
                }
                TEST_EQUAL(false, Server->GetIsConnected());
            }
        }
    }
}

void
QuicTestConnectValidServerCertificate(
    _In_ const QUIC_CREDENTIAL_CONFIG* Config
    )
{
    MsQuicRegistration Registration;
    TEST_TRUE(Registration.IsValid());

    MsQuicAlpn Alpn("MsQuicTest");

    MsQuicSettings Settings;
    Settings.SetIdleTimeoutMs(3000);

    MsQuicConfiguration ServerConfiguration(Registration, Alpn, Settings, *Config);
    TEST_TRUE(ServerConfiguration.IsValid());

    MsQuicCredentialConfig ClientCredConfig;
    ClientCredConfig.Flags &= ~QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
    MsQuicConfiguration ClientConfiguration(Registration, Alpn, Settings, ClientCredConfig);
    TEST_TRUE(ClientConfiguration.IsValid());

    QUIC_ADDRESS_FAMILY QuicAddrFamily = QUIC_ADDRESS_FAMILY_INET;

    {
        TestListener Listener(Registration, ListenerAcceptConnection, ServerConfiguration);
        TEST_TRUE(Listener.IsValid());
        QuicAddr ServerLocalAddr(QuicAddrFamily);
        TEST_QUIC_SUCCEEDED(Listener.Start(Alpn, &ServerLocalAddr.SockAddr));

        TEST_QUIC_SUCCEEDED(Listener.GetLocalAddr(ServerLocalAddr));

        {
            UniquePtr<TestConnection> Server;
            ServerAcceptContext ServerAcceptCtx((TestConnection**)&Server);
            Listener.Context = &ServerAcceptCtx;

            {
                TestConnection Client(Registration);
                TEST_TRUE(Client.IsValid());
                Client.SetSslKeyLogFilePath();

                TEST_QUIC_SUCCEEDED(
                    Client.Start(
                        ClientConfiguration,
                        QuicAddrFamily,
                        QUIC_TEST_LOOPBACK_FOR_AF(
                            QuicAddrGetFamily(&ServerLocalAddr.SockAddr)),
                        ServerLocalAddr.GetPort()));

                if (!Client.WaitForConnectionComplete()) {
                    return;
                }
                TEST_EQUAL(true, Client.GetIsConnected());

                TEST_NOT_EQUAL(nullptr, Server);
                Server->SetSslKeyLogFilePath();
                if (!Server->WaitForConnectionComplete()) {
                    return;
                }
                TEST_EQUAL(true, Server->GetIsConnected());
            }
        }
    }
}

void
QuicTestConnectValidClientCertificate(
    _In_ const QUIC_CREDENTIAL_CONFIG* Config
    )
{
    MsQuicRegistration Registration;
    TEST_TRUE(Registration.IsValid());

    MsQuicAlpn Alpn("MsQuicTest");

    MsQuicSettings Settings;
    Settings.SetIdleTimeoutMs(3000);

    MsQuicConfiguration ServerConfiguration(Registration, Alpn, Settings, ServerSelfSignedCredConfigClientAuth);
    TEST_TRUE(ServerConfiguration.IsValid());

    MsQuicCredentialConfig ClientCredConfig;
    MsQuicConfiguration ClientConfiguration(Registration, Alpn, Settings, *Config);
    TEST_TRUE(ClientConfiguration.IsValid());

    QUIC_ADDRESS_FAMILY QuicAddrFamily = QUIC_ADDRESS_FAMILY_INET;

    {
        TestListener Listener(Registration, ListenerAcceptConnection, ServerConfiguration);
        TEST_TRUE(Listener.IsValid());
        QuicAddr ServerLocalAddr(QuicAddrFamily);
        TEST_QUIC_SUCCEEDED(Listener.Start(Alpn, &ServerLocalAddr.SockAddr));

        TEST_QUIC_SUCCEEDED(Listener.GetLocalAddr(ServerLocalAddr));

        {
            UniquePtr<TestConnection> Server;
            ServerAcceptContext ServerAcceptCtx((TestConnection**)&Server);
            ServerAcceptCtx.AddExpectedClientCertValidationResult(QUIC_STATUS_SUCCESS);
            Listener.Context = &ServerAcceptCtx;

            {
                TestConnection Client(Registration);
                TEST_TRUE(Client.IsValid());
                Client.SetSslKeyLogFilePath();

                TEST_QUIC_SUCCEEDED(
                    Client.Start(
                        ClientConfiguration,
                        QuicAddrFamily,
                        QUIC_TEST_LOOPBACK_FOR_AF(
                            QuicAddrGetFamily(&ServerLocalAddr.SockAddr)),
                        ServerLocalAddr.GetPort()));

                if (!Client.WaitForConnectionComplete()) {
                    return;
                }
                TEST_EQUAL(true, Client.GetIsConnected());

                TEST_NOT_EQUAL(nullptr, Server);
                Server->SetSslKeyLogFilePath();
                if (!Server->WaitForConnectionComplete()) {
                    return;
                }
                TEST_EQUAL(true, Server->GetIsConnected());
            }
        }
    }
}

void
QuicTestConnectExpiredClientCertificate(
    _In_ const QUIC_CREDENTIAL_CONFIG* Config
    )
{
    MsQuicRegistration Registration;
    TEST_TRUE(Registration.IsValid());

    MsQuicAlpn Alpn("MsQuicTest");

    MsQuicSettings Settings;
    Settings.SetIdleTimeoutMs(3000);

    MsQuicConfiguration ServerConfiguration(Registration, Alpn, Settings, ServerSelfSignedCredConfigClientAuth);
    TEST_TRUE(ServerConfiguration.IsValid());

    MsQuicCredentialConfig ClientCredConfig;
    MsQuicConfiguration ClientConfiguration(Registration, Alpn, Settings, *Config);
    TEST_TRUE(ClientConfiguration.IsValid());

    QUIC_ADDRESS_FAMILY QuicAddrFamily = QUIC_ADDRESS_FAMILY_INET;

    {
        TestListener Listener(Registration, ListenerAcceptConnection, ServerConfiguration);
        TEST_TRUE(Listener.IsValid());
        QuicAddr ServerLocalAddr(QuicAddrFamily);
        TEST_QUIC_SUCCEEDED(Listener.Start(Alpn, &ServerLocalAddr.SockAddr));

        TEST_QUIC_SUCCEEDED(Listener.GetLocalAddr(ServerLocalAddr));

        {
            UniquePtr<TestConnection> Server;
            ServerAcceptContext ServerAcceptCtx((TestConnection**)&Server);
            ServerAcceptCtx.AddExpectedClientCertValidationResult(QUIC_STATUS_CERT_EXPIRED);
            Listener.Context = &ServerAcceptCtx;

            {
                TestConnection Client(Registration);
                TEST_TRUE(Client.IsValid());
                Client.SetSslKeyLogFilePath();

                TEST_QUIC_SUCCEEDED(
                    Client.Start(
                        ClientConfiguration,
                        QuicAddrFamily,
                        QUIC_TEST_LOOPBACK_FOR_AF(
                            QuicAddrGetFamily(&ServerLocalAddr.SockAddr)),
                        ServerLocalAddr.GetPort()));

                //
                // This test expects the server to accept the client
                // cert even though it gives a validation error.
                //
                if (!Client.WaitForConnectionComplete()) {
                    return;
                }
                TEST_EQUAL(true, Client.GetIsConnected());

                TEST_NOT_EQUAL(nullptr, Server);
                Server->SetSslKeyLogFilePath();
                if (!Server->WaitForConnectionComplete()) {
                    return;
                }
                TEST_EQUAL(true, Server->GetIsConnected());
            }
        }
    }
}

struct LoadBalancedServer {
    QuicAddr PublicAddress;
    QuicAddr* PrivateAddresses {nullptr};
    QUIC_TICKET_KEY_CONFIG KeyConfig;
    MsQuicConfiguration** Configurations {nullptr};
    MsQuicAutoAcceptListener** Listeners {nullptr};
    uint32_t ListenerCount;
    LoadBalancerHelper* LoadBalancer {nullptr};
    QUIC_STATUS InitStatus {QUIC_STATUS_INVALID_PARAMETER}; // Only hit in ListenerCount == 0 scenario
    LoadBalancedServer(
        _In_ const MsQuicRegistration& Registration,
        _In_ QUIC_ADDRESS_FAMILY QuicAddrFamily = QUIC_ADDRESS_FAMILY_UNSPEC,
        _In_ MsQuicConnectionCallback* ConnectionHandler = MsQuicConnection::NoOpCallback,
        _In_ uint32_t ListenerCount = 2
        ) noexcept :
        PublicAddress(QuicAddrFamily, (uint16_t)443), PrivateAddresses(new(std::nothrow) QuicAddr[ListenerCount]),
        Configurations(new(std::nothrow) MsQuicConfiguration*[ListenerCount]),
        Listeners(new(std::nothrow) MsQuicAutoAcceptListener*[ListenerCount]), ListenerCount(ListenerCount) {
        CxPlatRandom(sizeof(KeyConfig), &KeyConfig);
        KeyConfig.MaterialLength = sizeof(KeyConfig.Material);
        CxPlatZeroMemory(Configurations, sizeof(MsQuicConfiguration*) * ListenerCount);
        CxPlatZeroMemory(Listeners, sizeof(MsQuicAutoAcceptListener*) * ListenerCount);
        MsQuicSettings Settings;
        Settings.SetServerResumptionLevel(QUIC_SERVER_RESUME_AND_ZERORTT);
        if (UseDuoNic) {
            QuicAddrSetToDuoNic(&PublicAddress.SockAddr);
        } else {
            QuicAddrSetToLoopback(&PublicAddress.SockAddr);
        }
        for (uint32_t i = 0; i < ListenerCount; ++i) {
            PrivateAddresses[i] = QuicAddr(QuicAddrFamily);
            if (UseDuoNic) {
                QuicAddrSetToDuoNic(&PrivateAddresses[i].SockAddr);
            } else {
                QuicAddrSetToLoopback(&PrivateAddresses[i].SockAddr);
            }
            Configurations[i] = new(std::nothrow) MsQuicConfiguration(Registration, "MsQuicTest", Settings, ServerSelfSignedCredConfig);
            TEST_QUIC_SUCCEEDED(InitStatus = Configurations[i]->GetInitStatus());
            TEST_QUIC_SUCCEEDED(InitStatus = Configurations[i]->SetTicketKey(&KeyConfig));
            Listeners[i] = new(std::nothrow) MsQuicAutoAcceptListener(Registration, *Configurations[i], ConnectionHandler);
            TEST_QUIC_SUCCEEDED(InitStatus = Listeners[i]->GetInitStatus());
            TEST_QUIC_SUCCEEDED(InitStatus = Listeners[i]->Start("MsQuicTest", &PrivateAddresses[i].SockAddr));
            TEST_QUIC_SUCCEEDED(InitStatus = Listeners[i]->GetLocalAddr(PrivateAddresses[i]));
        }
        LoadBalancer = new(std::nothrow) LoadBalancerHelper(PublicAddress.SockAddr, (QUIC_ADDR*)PrivateAddresses, ListenerCount);
    }
    ~LoadBalancedServer() noexcept {
        delete LoadBalancer;
        for (uint32_t i = 0; i < ListenerCount; ++i) {
            delete Listeners[i];
            delete Configurations[i];
        }
        delete[] Listeners;
        delete[] Configurations;
        delete[] PrivateAddresses;
    }
    QUIC_STATUS GetInitStatus() const noexcept { return InitStatus; }
    void ValidateLoadBalancing() const noexcept {
        for (uint32_t i = 0; i < ListenerCount; ++i) {
            TEST_TRUE(Listeners[i]->AcceptedConnectionCount != 0);
        }
    }
};

void
QuicTestLoadBalancedHandshake(
    _In_ int Family
    )
{
    MsQuicRegistration Registration(true);
    TEST_QUIC_SUCCEEDED(Registration.GetInitStatus());
    if (QuitTestIsFeatureSupported(CXPLAT_DATAPATH_FEATURE_RAW)) {
        return; // TODO - QUIC_STATUS_NOT_SUPPORTED
    }

    MsQuicConfiguration ClientConfiguration(Registration, "MsQuicTest", MsQuicCredentialConfig());
    TEST_QUIC_SUCCEEDED(ClientConfiguration.GetInitStatus());

    QUIC_ADDRESS_FAMILY QuicAddrFamily = (Family == 4) ? QUIC_ADDRESS_FAMILY_INET : QUIC_ADDRESS_FAMILY_INET6;
    LoadBalancedServer Listeners(Registration, QuicAddrFamily, MsQuicConnection::SendResumptionCallback, 3);
    TEST_QUIC_SUCCEEDED(Listeners.GetInitStatus());

    QuicAddr ConnLocalAddr(QuicAddrFamily, false);
    uint32_t ResumptionTicketLength = 0;
    uint8_t* ResumptionTicket = nullptr;
    bool SchannelMode = false; // Only determined on first resumed connection.
    ConnLocalAddr.SetPort(33667); // Randomly chosen!
    for (uint32_t i = 0; i < 100; ++i) {
        MsQuicConnection Connection(Registration);
        TEST_QUIC_SUCCEEDED(Connection.GetInitStatus());
        bool TryingResumption = false;
        if (ResumptionTicket) {
            TEST_QUIC_SUCCEEDED(Connection.SetResumptionTicket(ResumptionTicket, ResumptionTicketLength));
            delete[] ResumptionTicket;
            ResumptionTicket = nullptr;
            TryingResumption = true;
        }
        TEST_QUIC_SUCCEEDED(Connection.SetLocalAddr(ConnLocalAddr));
        TEST_QUIC_SUCCEEDED(Connection.Start(ClientConfiguration, Listeners.PublicAddress.GetFamily(), QUIC_TEST_LOOPBACK_FOR_AF(Listeners.PublicAddress.GetFamily()), Listeners.PublicAddress.GetPort()));
        TEST_TRUE(Connection.HandshakeCompleteEvent.WaitTimeout(TestWaitTimeout));
        if (!Connection.HandshakeComplete) {
#ifdef WSAEACCES
            TEST_TRUE(
                Connection.TransportShutdownStatus == QUIC_STATUS_ADDRESS_IN_USE ||
                Connection.TransportShutdownStatus == HRESULT_FROM_WIN32(WSAEACCES));
#else
            TEST_TRUE(Connection.TransportShutdownStatus == QUIC_STATUS_ADDRESS_IN_USE);
#endif

        } else {
            if (SchannelMode) {
                //
                // HACK: Schannel reuses tickets, so it always resumes. Also, no
                // point in waiting for a ticket because it won't send it.
                //
                TEST_TRUE(Connection.HandshakeResumed);

            } else {
                TEST_TRUE(Connection.HandshakeResumed == TryingResumption);
                if (!Connection.ResumptionTicketReceivedEvent.WaitTimeout(TestWaitTimeout)) {
                    if (Connection.HandshakeResumed) {
                        SchannelMode = true; // Schannel doesn't send tickets on resumed connections.
                        ResumptionTicket = nullptr;
                    } else {
                        TEST_FAILURE("Timeout waiting for resumption ticket");
                        return;
                    }
                } else {
                    TEST_TRUE(Connection.ResumptionTicket != nullptr);
                    ResumptionTicketLength = Connection.ResumptionTicketLength;
                    ResumptionTicket = Connection.ResumptionTicket;
                    Connection.ResumptionTicket = nullptr;
                }
            }
            Connection.Shutdown(0); // Best effort start peer shutdown
        }
        ConnLocalAddr.IncrementPort();
    }
    delete[] ResumptionTicket;
    Listeners.ValidateLoadBalancing();
}

void
QuicTestClientSharedLocalPort(
    _In_ int Family
    )
{
    MsQuicRegistration Registration(true);
    TEST_QUIC_SUCCEEDED(Registration.GetInitStatus());

    MsQuicConfiguration ServerConfiguration(Registration, "MsQuicTest", ServerSelfSignedCredConfig);
    TEST_QUIC_SUCCEEDED(ServerConfiguration.GetInitStatus());

    MsQuicConfiguration ClientConfiguration(Registration, "MsQuicTest", MsQuicCredentialConfig());
    TEST_QUIC_SUCCEEDED(ClientConfiguration.GetInitStatus());

    QUIC_ADDRESS_FAMILY QuicAddrFamily = (Family == 4) ? QUIC_ADDRESS_FAMILY_INET : QUIC_ADDRESS_FAMILY_INET6;
    QuicAddr ServerLocalAddr(QuicAddrFamily);

    MsQuicAutoAcceptListener Listener1(Registration, ServerConfiguration, MsQuicConnection::NoOpCallback);
    TEST_QUIC_SUCCEEDED(Listener1.Start("MsQuicTest", &ServerLocalAddr.SockAddr));
    TEST_QUIC_SUCCEEDED(Listener1.GetInitStatus());
    QuicAddr Server1LocalAddr;
    TEST_QUIC_SUCCEEDED(Listener1.GetLocalAddr(Server1LocalAddr));

    MsQuicAutoAcceptListener Listener2(Registration, ServerConfiguration, MsQuicConnection::NoOpCallback);
    TEST_QUIC_SUCCEEDED(Listener2.Start("MsQuicTest", &ServerLocalAddr.SockAddr));
    TEST_QUIC_SUCCEEDED(Listener2.GetInitStatus());
    QuicAddr Server2LocalAddr;
    TEST_QUIC_SUCCEEDED(Listener2.GetLocalAddr(Server2LocalAddr));

    MsQuicConnection Connection1(Registration);
    TEST_QUIC_SUCCEEDED(Connection1.GetInitStatus());
    TEST_QUIC_SUCCEEDED(Connection1.SetShareUdpBinding());
    TEST_QUIC_SUCCEEDED(Connection1.Start(ClientConfiguration, Server1LocalAddr.GetFamily(), QUIC_TEST_LOOPBACK_FOR_AF(Server1LocalAddr.GetFamily()), Server1LocalAddr.GetPort()));
    TEST_TRUE(Connection1.HandshakeCompleteEvent.WaitTimeout(TestWaitTimeout));
    TEST_TRUE(Connection1.HandshakeComplete);
    QuicAddr Client1LocalAddr;
    TEST_QUIC_SUCCEEDED(Connection1.GetLocalAddr(Client1LocalAddr));

    MsQuicConnection Connection2(Registration);
    TEST_QUIC_SUCCEEDED(Connection2.GetInitStatus());
    TEST_QUIC_SUCCEEDED(Connection2.SetShareUdpBinding());
    TEST_QUIC_SUCCEEDED(Connection2.SetLocalAddr(Client1LocalAddr));
    TEST_QUIC_SUCCEEDED(Connection2.Start(ClientConfiguration, Server1LocalAddr.GetFamily(), QUIC_TEST_LOOPBACK_FOR_AF(Server1LocalAddr.GetFamily()), Server1LocalAddr.GetPort()));
    TEST_TRUE(Connection2.HandshakeCompleteEvent.WaitTimeout(TestWaitTimeout));
    TEST_TRUE(Connection2.HandshakeComplete);
}

bool
GetTestInterfaceIndices(
    _In_ QUIC_ADDRESS_FAMILY QuicAddrFamily,
    _Out_ uint32_t& LoopbackInterfaceIndex,
    _Out_ uint32_t& OtherInterfaceIndex
    )
{
    CXPLAT_ADAPTER_ADDRESS* Addresses = nullptr;
    uint32_t AddressesCount = 0;
    if (CxPlatDataPathGetLocalAddresses(nullptr, &Addresses, &AddressesCount) == QUIC_STATUS_NOT_SUPPORTED) {
        return false; // Not currently supported by this platform.
    }

    for (uint32_t i = 0; i < AddressesCount; ++i) {
        if (Addresses[i].OperationStatus == CXPLAT_OPERATION_STATUS_UP &&
            QuicAddrGetFamily(&Addresses[i].Address) == QuicAddrFamily) {
            if (Addresses[i].InterfaceType == CXPLAT_IF_TYPE_SOFTWARE_LOOPBACK &&
                LoopbackInterfaceIndex == UINT32_MAX) {
                LoopbackInterfaceIndex = Addresses[i].InterfaceIndex;
            }
            if (Addresses[i].InterfaceType != CXPLAT_IF_TYPE_SOFTWARE_LOOPBACK &&
                OtherInterfaceIndex == UINT32_MAX) {
                OtherInterfaceIndex = Addresses[i].InterfaceIndex;
            }
        }
    }

    CXPLAT_FREE(Addresses, QUIC_POOL_DATAPATH_ADDRESSES);

    return LoopbackInterfaceIndex != UINT32_MAX && OtherInterfaceIndex != UINT32_MAX;
}

void
QuicTestInterfaceBinding(
    _In_ int Family
    )
{
    QUIC_ADDRESS_FAMILY QuicAddrFamily = (Family == 4) ? QUIC_ADDRESS_FAMILY_INET : QUIC_ADDRESS_FAMILY_INET6;
    uint32_t LoopbackInterfaceIndex = UINT32_MAX;
    uint32_t OtherInterfaceIndex = UINT32_MAX;
    if (!GetTestInterfaceIndices(QuicAddrFamily, LoopbackInterfaceIndex, OtherInterfaceIndex)) {
        return; // Not supported
    }

    MsQuicRegistration Registration(true);
    TEST_QUIC_SUCCEEDED(Registration.GetInitStatus());

    MsQuicConfiguration ServerConfiguration(Registration, "MsQuicTest", ServerSelfSignedCredConfig);
    TEST_QUIC_SUCCEEDED(ServerConfiguration.GetInitStatus());

    MsQuicConfiguration ClientConfiguration(Registration, "MsQuicTest", MsQuicCredentialConfig());
    TEST_QUIC_SUCCEEDED(ClientConfiguration.GetInitStatus());

    QuicAddr ServerLocalAddr(QuicAddrFamily);
    MsQuicAutoAcceptListener Listener(Registration, ServerConfiguration, MsQuicConnection::NoOpCallback);
    TEST_QUIC_SUCCEEDED(Listener.Start("MsQuicTest", &ServerLocalAddr.SockAddr));
    TEST_QUIC_SUCCEEDED(Listener.GetInitStatus());
    TEST_QUIC_SUCCEEDED(Listener.GetLocalAddr(ServerLocalAddr));

    MsQuicConnection Connection1(Registration);
    TEST_QUIC_SUCCEEDED(Connection1.GetInitStatus());
    TEST_QUIC_SUCCEEDED(Connection1.SetLocalInterface(LoopbackInterfaceIndex));
    TEST_QUIC_SUCCEEDED(Connection1.Start(ClientConfiguration, ServerLocalAddr.GetFamily(), QUIC_TEST_LOOPBACK_FOR_AF(ServerLocalAddr.GetFamily()), ServerLocalAddr.GetPort()));
    TEST_TRUE(Connection1.HandshakeCompleteEvent.WaitTimeout(TestWaitTimeout));
    TEST_TRUE(Connection1.HandshakeComplete);

    MsQuicConnection Connection2(Registration);
    TEST_QUIC_SUCCEEDED(Connection2.GetInitStatus());
    TEST_QUIC_SUCCEEDED(Connection2.SetLocalInterface(OtherInterfaceIndex));
    TEST_QUIC_SUCCEEDED(Connection2.Start(ClientConfiguration, ServerLocalAddr.GetFamily(), QUIC_TEST_LOOPBACK_FOR_AF(ServerLocalAddr.GetFamily()), ServerLocalAddr.GetPort()));
    Connection2.HandshakeCompleteEvent.WaitTimeout(TestWaitTimeout);
    TEST_TRUE(!Connection2.HandshakeComplete);
}

struct ListenerContext {
    bool DosModeChangeEventReceived {false};
    CxPlatEvent DosModeChanged;
    static QUIC_STATUS ListenerCallback(_In_ MsQuicListener *, _In_opt_ void* Context, _Inout_ QUIC_LISTENER_EVENT* Event) {
        auto This = static_cast<ListenerContext *>(Context);
        if (Event->Type == QUIC_LISTENER_EVENT_DOS_MODE_CHANGED) {
            This->DosModeChanged.Set();
            This->DosModeChangeEventReceived = true;
        }
        return QUIC_STATUS_SUCCESS;
    }
};

void
QuicTestRetryMemoryLimitConnect(
    _In_ int Family
    )
{
    ListenerContext Context;
    QUIC_ADDRESS_FAMILY QuicAddrFamily = (Family == 4) ? QUIC_ADDRESS_FAMILY_INET : QUIC_ADDRESS_FAMILY_INET6;
    uint32_t LoopbackInterfaceIndex = UINT32_MAX;
    uint32_t OtherInterfaceIndex = UINT32_MAX;
    const uint16_t RetryMemoryLimit = 0;
    if (!GetTestInterfaceIndices(QuicAddrFamily, LoopbackInterfaceIndex, OtherInterfaceIndex)) {
        return; // Not supported
    }

    MsQuicRegistration Registration(true);
    TEST_QUIC_SUCCEEDED(Registration.GetInitStatus());
    TEST_TRUE(Registration.IsValid());

    QuicAddr ServerLocalAddr(QuicAddrFamily);
    MsQuicListener Listener(Registration, CleanUpManual, ListenerContext::ListenerCallback, &Context);

    TEST_QUIC_SUCCEEDED(
        MsQuic->SetParam(
            NULL,
            QUIC_PARAM_GLOBAL_RETRY_MEMORY_PERCENT,
            sizeof(RetryMemoryLimit),
            &RetryMemoryLimit));

    uint8_t buffer[1] = {1};
    TEST_QUIC_SUCCEEDED(
        MsQuic->SetParam(
            Listener.Handle,
            QUIC_PARAM_DOS_MODE_EVENTS,
            sizeof(buffer),
            &buffer));

    uint32_t Length = 65535;
    buffer[0] = {0};
    TEST_QUIC_SUCCEEDED(
        Listener.GetParam(
            QUIC_PARAM_DOS_MODE_EVENTS,
            &Length,
            &buffer));
    TEST_EQUAL(Length, sizeof(BOOLEAN)); //sizeof (((QUIC_LISTENER *)0)->DosModeEventsEnabled)
    TEST_EQUAL(buffer[0], 1);

    TEST_QUIC_SUCCEEDED(Listener.Start("MsQuicTest", &ServerLocalAddr.SockAddr));
    TEST_QUIC_SUCCEEDED(Listener.GetInitStatus());
    TEST_QUIC_SUCCEEDED(Listener.GetLocalAddr(ServerLocalAddr));
    TEST_TRUE(Context.DosModeChanged.WaitTimeout(TestWaitTimeout));
    TEST_TRUE(Context.DosModeChangeEventReceived);
}

#ifdef QUIC_API_ENABLE_PREVIEW_FEATURES
void
QuicTestCibirExtension(
    _In_ int Family,
    _In_ uint8_t Mode // server = &1, client = &2
    )
{
    const uint8_t CibirId[] = { 0 /* offset */, 4, 3, 2, 1 };
    const uint8_t CibirIdLength = sizeof(CibirId);
    const bool ShouldConnnect = !!(Mode & 1) == !!(Mode & 2);

    MsQuicRegistration Registration(true);
    TEST_QUIC_SUCCEEDED(Registration.GetInitStatus());

    MsQuicConfiguration ServerConfiguration(Registration, "MsQuicTest", ServerSelfSignedCredConfig);
    TEST_QUIC_SUCCEEDED(ServerConfiguration.GetInitStatus());

    MsQuicConfiguration ClientConfiguration(Registration, "MsQuicTest", MsQuicCredentialConfig());
    TEST_QUIC_SUCCEEDED(ClientConfiguration.GetInitStatus());

    QUIC_ADDRESS_FAMILY QuicAddrFamily = (Family == 4) ? QUIC_ADDRESS_FAMILY_INET : QUIC_ADDRESS_FAMILY_INET6;
    QuicAddr ServerLocalAddr(QuicAddrFamily);
    MsQuicAutoAcceptListener Listener(Registration, ServerConfiguration, MsQuicConnection::NoOpCallback);
    if (Mode & 1) {
        TEST_QUIC_SUCCEEDED(Listener.SetCibirId(CibirId, CibirIdLength));
    }
    TEST_QUIC_SUCCEEDED(Listener.Start("MsQuicTest", &ServerLocalAddr.SockAddr));
    TEST_QUIC_SUCCEEDED(Listener.GetInitStatus());
    TEST_QUIC_SUCCEEDED(Listener.GetLocalAddr(ServerLocalAddr));

    MsQuicConnection Connection(Registration);
    TEST_QUIC_SUCCEEDED(Connection.GetInitStatus());
    if (Mode & 2) {
        TEST_QUIC_SUCCEEDED(Connection.SetShareUdpBinding());
        TEST_QUIC_SUCCEEDED(Connection.SetCibirId(CibirId, CibirIdLength));
    }
    if (!ShouldConnnect) {
        // TODO - Set expected transport error
    }
    TEST_QUIC_SUCCEEDED(Connection.Start(ClientConfiguration, ServerLocalAddr.GetFamily(), QUIC_TEST_LOOPBACK_FOR_AF(ServerLocalAddr.GetFamily()), ServerLocalAddr.GetPort()));
    Connection.HandshakeCompleteEvent.WaitTimeout(TestWaitTimeout);
    TEST_EQUAL(Connection.HandshakeComplete, ShouldConnnect);
}

void
QuicTestResumptionAcrossVersions()
{
    MsQuicRegistration Registration;
    TEST_QUIC_SUCCEEDED(Registration.GetInitStatus());
    uint32_t FirstClientVersions[] = {QUIC_VERSION_1_H};
    uint32_t SecondClientVersions[] = {QUIC_VERSION_2_H};
    MsQuicVersionSettings VersionSettings{};

    MsQuicAlpn Alpn("MsQuicTest");

    MsQuicSettings ServerSettings;
    ServerSettings.SetServerResumptionLevel(QUIC_SERVER_RESUME_ONLY);

    MsQuicConfiguration ServerConfiguration(Registration, Alpn, ServerSettings, ServerSelfSignedCredConfig);
    TEST_QUIC_SUCCEEDED(ServerConfiguration.GetInitStatus());

    MsQuicConfiguration ClientConfiguration(Registration, Alpn, MsQuicCredentialConfig());
    TEST_QUIC_SUCCEEDED(ClientConfiguration.GetInitStatus());

    VersionSettings.SetAllVersionLists(FirstClientVersions, ARRAYSIZE(FirstClientVersions));
    TEST_QUIC_SUCCEEDED(ClientConfiguration.SetVersionSettings(VersionSettings));

    QUIC_ADDRESS_FAMILY QuicAddrFamily = QUIC_ADDRESS_FAMILY_INET;
    QUIC_BUFFER* ResumptionTicket = nullptr;

    QuicTestPrimeResumption(QuicAddrFamily, Registration, ServerConfiguration, ClientConfiguration, &ResumptionTicket);
    if (ResumptionTicket == nullptr) {
        return;
    }

    {
        TestListener Listener(Registration, ListenerAcceptConnection, ServerConfiguration);
        TEST_TRUE(Listener.IsValid());
        QuicAddr ServerLocalAddr(QuicAddrFamily);
        TEST_QUIC_SUCCEEDED(Listener.Start(Alpn, &ServerLocalAddr.SockAddr));

        TEST_QUIC_SUCCEEDED(Listener.GetLocalAddr(ServerLocalAddr));

        {
            UniquePtr<TestConnection> Server;
            ServerAcceptContext ServerAcceptCtx((TestConnection**)&Server);
            Listener.Context = &ServerAcceptCtx;
            {
                TestConnection Client(Registration);
                TEST_TRUE(Client.IsValid());
                Client.SetSslKeyLogFilePath();

                VersionSettings.SetAllVersionLists(SecondClientVersions, ARRAYSIZE(SecondClientVersions));
                TEST_QUIC_SUCCEEDED(ClientConfiguration.SetVersionSettings(VersionSettings));
                TEST_QUIC_SUCCEEDED(Client.SetResumptionTicket(ResumptionTicket));
                CXPLAT_FREE(ResumptionTicket, QUIC_POOL_TEST);
                Client.SetExpectedResumed(false);

                if (UseDuoNic) {
                    QuicAddr RemoteAddr{QuicAddrGetFamily(&ServerLocalAddr.SockAddr), ServerLocalAddr.GetPort()};
                    QuicAddrSetToDuoNic(&RemoteAddr.SockAddr);
                    TEST_QUIC_SUCCEEDED(Client.SetRemoteAddr(RemoteAddr));
                }

                TEST_QUIC_SUCCEEDED(
                    Client.Start(
                        ClientConfiguration,
                        QuicAddrFamily,
                        QUIC_LOCALHOST_FOR_AF(QuicAddrFamily),
                        ServerLocalAddr.GetPort()));

                if (!Client.WaitForConnectionComplete()) {
                    return;
                }
                TEST_TRUE(Client.GetIsConnected());

                TEST_NOT_EQUAL(nullptr, Server);
                Server->SetSslKeyLogFilePath();
                if (!Server->WaitForConnectionComplete()) {
                    return;
                }
                TEST_TRUE(Server->GetIsConnected());
                TEST_FALSE(Client.GetResumed());
                TEST_FALSE(Server->GetResumed());
            }
        }
    }
}
#endif // QUIC_API_ENABLE_PREVIEW_FEATURES

void
QuicTestClientBlockedSourcePort(
    _In_ int Family
    )
{
    MsQuicRegistration Registration(true);
    TEST_QUIC_SUCCEEDED(Registration.GetInitStatus());

    MsQuicConfiguration ServerConfiguration(Registration, "MsQuicTest", ServerSelfSignedCredConfig);
    TEST_QUIC_SUCCEEDED(ServerConfiguration.GetInitStatus());

    MsQuicSettings ClientSettings;
    ClientSettings.SetDisconnectTimeoutMs(500);

    MsQuicConfiguration ClientConfiguration(Registration, "MsQuicTest", ClientSettings, MsQuicCredentialConfig());
    TEST_QUIC_SUCCEEDED(ClientConfiguration.GetInitStatus());

    const QUIC_ADDRESS_FAMILY QuicAddrFamily = (Family == 4) ? QUIC_ADDRESS_FAMILY_INET : QUIC_ADDRESS_FAMILY_INET6;
    QuicAddr ServerLocalAddr(QuicAddrFamily);

    MsQuicAutoAcceptListener Listener(Registration, ServerConfiguration, MsQuicConnection::NoOpCallback);
    TEST_QUIC_SUCCEEDED(Listener.Start("MsQuicTest", &ServerLocalAddr.SockAddr));
    TEST_QUIC_SUCCEEDED(Listener.GetInitStatus());
    TEST_QUIC_SUCCEEDED(Listener.GetLocalAddr(ServerLocalAddr));

    MsQuicConnection Client(Registration);
    TEST_QUIC_SUCCEEDED(Client.GetInitStatus());
    TEST_QUIC_SUCCEEDED(Client.SetLocalAddr(QuicAddr(QuicAddrFamily, (uint16_t)11211 /* memcache port */)));
    TEST_QUIC_SUCCEEDED(Client.Start(ClientConfiguration, QuicAddrFamily, QUIC_TEST_LOOPBACK_FOR_AF(QuicAddrFamily), ServerLocalAddr.GetPort()));
    TEST_TRUE(Client.HandshakeCompleteEvent.WaitTimeout(TestWaitTimeout));
    TEST_TRUE(!Client.HandshakeComplete);
    TEST_EQUAL(Client.TransportShutdownStatus, QUIC_STATUS_CONNECTION_TIMEOUT);

    QUIC_LISTENER_STATISTICS ListenerStats {0};
    TEST_QUIC_SUCCEEDED(Listener.GetStatistics(ListenerStats));
    TEST_TRUE(ListenerStats.BindingRecvDroppedPackets > 0);
}

void
QuicTestChangeAlpn(
    void
    )
{
    MsQuicRegistration Registration;
    TEST_TRUE(Registration.IsValid());

    // Success cases
    {
        const char* FirstAlpns[] = {"quic1", "quic1", "VerifyThisAsQuicALPN", "VerifyThisAsQuicALPN", "quic1"};
        const char* SecondAlpns[] = {"MsQuicTest", "MsQuicVerifyThisAsQuicALPN", "MsQuicTest", "MsQuicVerifyThisAsQuicALPN", "MsQuicTest"};
        for (uint32_t idx = 0; idx < ARRAYSIZE(FirstAlpns); ++idx) {
            MsQuicAlpn Alpn(FirstAlpns[idx], SecondAlpns[idx]);

            MsQuicAlpn NewAlpn(SecondAlpns[idx]);

            MsQuicSettings Settings;
            Settings.SetIdleTimeoutMs(3000);

            MsQuicConfiguration ServerConfiguration(Registration, NewAlpn, Settings, ServerSelfSignedCredConfig);
            TEST_TRUE(ServerConfiguration.IsValid());

            MsQuicCredentialConfig ClientCredConfig;
            MsQuicConfiguration ClientConfiguration(Registration, Alpn, Settings, ClientCredConfig);
            TEST_TRUE(ClientConfiguration.IsValid());

            QUIC_ADDRESS_FAMILY QuicAddrFamily = QUIC_ADDRESS_FAMILY_INET;

            {
                TestListener Listener(Registration, ListenerAcceptConnection, ServerConfiguration);
                TEST_TRUE(Listener.IsValid());
                QuicAddr ServerLocalAddr(QuicAddrFamily);
                TEST_QUIC_SUCCEEDED(Listener.Start(Alpn, &ServerLocalAddr.SockAddr));

                TEST_QUIC_SUCCEEDED(Listener.GetLocalAddr(ServerLocalAddr));

                {
                    UniquePtr<TestConnection> Server;
                    ServerAcceptContext ServerAcceptCtx((TestConnection**)&Server);
                    Listener.Context = &ServerAcceptCtx;

                    {
                        TestConnection Client(Registration);
                        TEST_TRUE(Client.IsValid());
                        Client.SetSslKeyLogFilePath();

                        TEST_QUIC_SUCCEEDED(
                            Client.Start(
                                ClientConfiguration,
                                QuicAddrFamily,
                                QUIC_TEST_LOOPBACK_FOR_AF(
                                    QuicAddrGetFamily(&ServerLocalAddr.SockAddr)),
                                ServerLocalAddr.GetPort()));

                        if (!Client.WaitForConnectionComplete()) {
                            return;
                        }
                        TEST_TRUE(Client.GetIsConnected());

                        TEST_NOT_EQUAL(nullptr, Server);
                        Server->SetSslKeyLogFilePath();
                        if (!Server->WaitForConnectionComplete()) {
                            return;
                        }
                        TEST_TRUE(Server->GetIsConnected());

                        auto& AlpnBuffer = NewAlpn[0];

                        TEST_EQUAL(Server->GetNegotiatedAlpnLength(), AlpnBuffer.Length);
                        for (uint32_t i = 0; i < AlpnBuffer.Length; i++) {
                            TEST_EQUAL(Server->GetNegotiatedAlpn()[i], AlpnBuffer.Buffer[i]);
                        }

                        TEST_EQUAL(Client.GetNegotiatedAlpnLength(), AlpnBuffer.Length);
                        for (uint32_t i = 0; i < AlpnBuffer.Length; i++) {
                            TEST_EQUAL(Client.GetNegotiatedAlpn()[i], AlpnBuffer.Buffer[i]);
                        }
                    }
                }
            }
        }
    }
    // Failure cases
    {
        MsQuicAlpn Alpn("quic1", "MsQuicTest");

        MsQuicAlpn NewAlpn("MsQuicTest23");

        MsQuicSettings Settings;
        Settings.SetIdleTimeoutMs(3000);

        MsQuicConfiguration ServerConfiguration(Registration, NewAlpn, Settings, ServerSelfSignedCredConfig);
        TEST_TRUE(ServerConfiguration.IsValid());

        MsQuicCredentialConfig ClientCredConfig;
        MsQuicConfiguration ClientConfiguration(Registration, Alpn, Settings, ClientCredConfig);
        TEST_TRUE(ClientConfiguration.IsValid());

        QUIC_ADDRESS_FAMILY QuicAddrFamily = QUIC_ADDRESS_FAMILY_INET;

        {
            TestListener Listener(Registration, ListenerAcceptConnection, ServerConfiguration);
            TEST_TRUE(Listener.IsValid());
            QuicAddr ServerLocalAddr(QuicAddrFamily);
            TEST_QUIC_SUCCEEDED(Listener.Start(Alpn, &ServerLocalAddr.SockAddr));

            TEST_QUIC_SUCCEEDED(Listener.GetLocalAddr(ServerLocalAddr));

            {
                UniquePtr<TestConnection> Server;
                ServerAcceptContext ServerAcceptCtx((TestConnection**)&Server);
                ServerAcceptCtx.ExpectedTransportCloseStatus = QUIC_STATUS_ALPN_NEG_FAILURE;
                Listener.Context = &ServerAcceptCtx;

                {
                    TestConnection Client(Registration);
                    TEST_TRUE(Client.IsValid());

                    Client.SetExpectedTransportCloseStatus(QUIC_STATUS_ALPN_NEG_FAILURE);

                    TEST_QUIC_SUCCEEDED(
                        Client.Start(
                            ClientConfiguration,
                            QuicAddrFamily,
                            QUIC_TEST_LOOPBACK_FOR_AF(
                                QuicAddrGetFamily(&ServerLocalAddr.SockAddr)),
                            ServerLocalAddr.GetPort()));

                    if (!Client.WaitForConnectionComplete()) {
                        return;
                    }
                    TEST_FALSE(Client.GetIsConnected());

                    TEST_NOT_EQUAL(nullptr, Server);
                    if (!Server->WaitForConnectionComplete()) {
                        return;
                    }
                    TEST_FALSE(Server->GetIsConnected());
                }
            }
        }
    }
}

#ifdef QUIC_API_ENABLE_PREVIEW_FEATURES
_Function_class_(NEW_CONNECTION_CALLBACK)
static
bool
QUIC_API
ListenerAcceptConnectionTestTP(
    _In_ TestListener* Listener,
    _In_ HQUIC ConnectionHandle
    )
{
    ServerAcceptContext* AcceptContext = (ServerAcceptContext*)Listener->Context;
    const BOOLEAN Disable = TRUE;
    QUIC_STATUS Status;
    if (QUIC_FAILED(
        Status = MsQuic->SetParam(
            ConnectionHandle,
            QUIC_PARAM_CONN_DISABLE_VNE_TP_GENERATION,
            sizeof(Disable),
            &Disable))) {
        TEST_FAILURE("Failed to disable VNE TP generation, 0x%x", Status);
        return false;
    }
    if (AcceptContext->TestTP == nullptr) {
        TEST_FAILURE("Did you forget to set the test TP on the acceptcontext?");
        return false;
    }
    if (QUIC_FAILED(
        Status = MsQuic->SetParam(
            ConnectionHandle,
            QUIC_PARAM_CONN_TEST_TRANSPORT_PARAMETER,
            sizeof(*AcceptContext->TestTP),
            AcceptContext->TestTP))) {
        TEST_FAILURE("Failed to set test TP on connection, 0x%x", Status);
        return false;
    }
    return ListenerAcceptConnection(Listener, ConnectionHandle);
}

void
QuicTestCustomVNTP(
    _In_ bool TestServer,
    _In_ QUIC_PRIVATE_TRANSPORT_PARAMETER* TestTP
    )
{
    MsQuicRegistration Registration;
    TEST_QUIC_SUCCEEDED(Registration.GetInitStatus());

    MsQuicAlpn Alpn("MsQuicTest");

    ClearGlobalVersionListScope ClearVersionsScope;
    BOOLEAN Enabled = TRUE;
    TEST_QUIC_SUCCEEDED(
        MsQuic->SetParam(
            NULL,
            QUIC_PARAM_GLOBAL_VERSION_NEGOTIATION_ENABLED,
            sizeof(Enabled),
            &Enabled));

    MsQuicConfiguration ServerConfiguration(Registration, Alpn, ServerSelfSignedCredConfig);
    TEST_QUIC_SUCCEEDED(ServerConfiguration.GetInitStatus());

    MsQuicConfiguration ClientConfiguration(Registration, Alpn, MsQuicCredentialConfig());
    TEST_QUIC_SUCCEEDED(ClientConfiguration.GetInitStatus());

    QUIC_ADDRESS_FAMILY QuicAddrFamily = QUIC_ADDRESS_FAMILY_INET;

    {
        TestListener Listener(Registration, TestServer ? ListenerAcceptConnectionTestTP : ListenerAcceptConnection, ServerConfiguration);
        TEST_TRUE(Listener.IsValid());
        QuicAddr ServerLocalAddr(QuicAddrFamily);
        TEST_QUIC_SUCCEEDED(Listener.Start(Alpn, &ServerLocalAddr.SockAddr));

        TEST_QUIC_SUCCEEDED(Listener.GetLocalAddr(ServerLocalAddr));

        {
            UniquePtr<TestConnection> Server;
            ServerAcceptContext ServerAcceptCtx((TestConnection**)&Server);
            ServerAcceptCtx.ExpectedTransportCloseStatus = QUIC_STATUS_INTERNAL_ERROR;
            Listener.Context = &ServerAcceptCtx;
            ServerAcceptCtx.TestTP = TestTP;
            {
                TestConnection Client(Registration);
                if (!TestServer) {
                    Client.SetTestTransportParameter(TestTP);
                    BOOLEAN Disable = TRUE;
                    TEST_QUIC_SUCCEEDED(
                        MsQuic->SetParam(
                            Client.GetConnection(),
                            QUIC_PARAM_CONN_DISABLE_VNE_TP_GENERATION,
                            sizeof(Disable),
                            &Disable));
                }
                TEST_TRUE(Client.IsValid());
                Client.SetSslKeyLogFilePath();
                Client.SetExpectedTransportCloseStatus(QUIC_STATUS_INTERNAL_ERROR);

                TEST_QUIC_SUCCEEDED(
                    Client.Start(
                        ClientConfiguration,
                        QuicAddrFamily,
                        QUIC_TEST_LOOPBACK_FOR_AF(QuicAddrFamily),
                        ServerLocalAddr.GetPort()));

                if (!Client.WaitForConnectionComplete()) {
                    return;
                }
                TEST_FALSE(Client.GetIsConnected());

                if (TestServer) {
                    TEST_NOT_EQUAL(nullptr, Server);
                    Server->SetSslKeyLogFilePath();
                    if (!Server->WaitForConnectionComplete()) {
                        return;
                    }
                    TEST_FALSE(Server->GetIsConnected());
                }
            }

        }
    }
}

#define QUIC_TP_ID_VERSION_NEGOTIATION_EXT                  0x11

void
QuicTestVNTPOddSize(
    _In_ bool TestServer,
    _In_ uint16_t VNTPSize
    )
{

    QUIC_PRIVATE_TRANSPORT_PARAMETER TestTP;
    TestTP.Type = QUIC_TP_ID_VERSION_NEGOTIATION_EXT;
    TestTP.Length = VNTPSize;
    UniquePtr<uint8_t[]> TPData(
        VNTPSize ? new(std::nothrow) uint8_t[VNTPSize] : nullptr);
    TestTP.Buffer = TPData.get();

    if (VNTPSize > 0) {
        TEST_TRUE(TPData.get());
        CxPlatZeroMemory(TPData.get(), VNTPSize);
    }

    if (VNTPSize >= sizeof(uint32_t)) {
        uint32_t Latest = QUIC_VERSION_LATEST;
        //
        // Ensure that if a chosen_version can fit, it is a valid version.
        //
        CxPlatCopyMemory(TPData.get(), &Latest, sizeof(uint32_t));
    }

    QuicTestCustomVNTP(TestServer, &TestTP);
}

void
QuicTestVNTPChosenVersionMismatch(
    _In_ bool TestServer
    )
{
    const uint32_t VNTPSize = 8;

    QUIC_PRIVATE_TRANSPORT_PARAMETER TestTP;
    TestTP.Type = QUIC_TP_ID_VERSION_NEGOTIATION_EXT;
    TestTP.Length = VNTPSize;
    UniquePtr<uint8_t[]> TPData(new(std::nothrow) uint8_t[VNTPSize]);
    TestTP.Buffer = TPData.get();

    const uint32_t WrongVersion = QUIC_VERSION_MS_1;

    CxPlatCopyMemory(TPData.get(), &WrongVersion, sizeof(WrongVersion));
    CxPlatCopyMemory(TPData.get() + sizeof(WrongVersion), &WrongVersion, sizeof(WrongVersion));

    QuicTestCustomVNTP(TestServer, &TestTP);
}

void
QuicTestVNTPChosenVersionZero(
    _In_ bool TestServer
    )
{
    const uint32_t VNTPSize = 3 * sizeof(uint32_t);

    QUIC_PRIVATE_TRANSPORT_PARAMETER TestTP;
    TestTP.Type = QUIC_TP_ID_VERSION_NEGOTIATION_EXT;
    TestTP.Length = VNTPSize;
    UniquePtr<uint8_t[]> TPData(new(std::nothrow) uint8_t[VNTPSize]);
    TestTP.Buffer = TPData.get();

    const uint32_t ZeroVersion = 0;
    const uint32_t Version1 = QUIC_VERSION_1;
    const uint32_t Version2 = QUIC_VERSION_2;
    uint8_t* Cursor = TPData.get();

    CxPlatCopyMemory(Cursor, &ZeroVersion, sizeof(ZeroVersion));
    Cursor += sizeof(uint32_t);
    //
    // Use Version 2 when testing server because the server will perform
    // Compatible Version Negotiation, and if using Version1, the client
    // will fail for the wrong reason: Long Header Version/Chosen Version
    // mismatch.
    //
    CxPlatCopyMemory(Cursor, TestServer ? &Version2 : &Version1, sizeof(Version1));
    Cursor += sizeof(uint32_t);
    CxPlatCopyMemory(Cursor, &ZeroVersion, sizeof(ZeroVersion));

    QuicTestCustomVNTP(TestServer, &TestTP);
}

void
QuicTestVNTPOtherVersionZero(
    _In_ bool TestServer
    )
{
    const uint32_t VNTPSize = 3 * sizeof(uint32_t);

    QUIC_PRIVATE_TRANSPORT_PARAMETER TestTP;
    TestTP.Type = QUIC_TP_ID_VERSION_NEGOTIATION_EXT;
    TestTP.Length = VNTPSize;
    UniquePtr<uint8_t[]> TPData(new(std::nothrow) uint8_t[VNTPSize]);
    TestTP.Buffer = TPData.get();

    const uint32_t ZeroVersion = 0;
    const uint32_t Version1 = QUIC_VERSION_1;
    uint8_t* Cursor = TPData.get();

    CxPlatCopyMemory(Cursor, &Version1, sizeof(Version1));
    Cursor += sizeof(uint32_t);
    CxPlatCopyMemory(Cursor, &Version1, sizeof(Version1));
    Cursor += sizeof(uint32_t);
    CxPlatCopyMemory(Cursor, &ZeroVersion, sizeof(ZeroVersion));

    QuicTestCustomVNTP(TestServer, &TestTP);
}
#endif // QUIC_API_ENABLE_PREVIEW_FEATURES

void
QuicTestHandshakeSpecificLossPatterns(
    _In_ int Family,
    _In_ QUIC_CONGESTION_CONTROL_ALGORITHM CcAlgo
    )
{
    MsQuicRegistration Registration(true);
    TEST_QUIC_SUCCEEDED(Registration.GetInitStatus());

    MsQuicSettings Settings;
    Settings.SetIdleTimeoutMs(60000)
        .SetDisconnectTimeoutMs(60000)
        .SetInitialRttMs(20)
        .SetCongestionControlAlgorithm(CcAlgo);

    MsQuicConfiguration ServerConfiguration(Registration, "MsQuicTest", Settings, ServerSelfSignedCredConfig);
    TEST_QUIC_SUCCEEDED(ServerConfiguration.GetInitStatus());

    MsQuicConfiguration ClientConfiguration(Registration, "MsQuicTest", Settings, MsQuicCredentialConfig());
    TEST_QUIC_SUCCEEDED(ClientConfiguration.GetInitStatus());

    QuicAddr ServerLocalAddr((Family == 4) ? QUIC_ADDRESS_FAMILY_INET : QUIC_ADDRESS_FAMILY_INET6);
    MsQuicAutoAcceptListener Listener(Registration, ServerConfiguration, MsQuicConnection::NoOpCallback);
    TEST_QUIC_SUCCEEDED(Listener.Start("MsQuicTest", &ServerLocalAddr.SockAddr));
    TEST_QUIC_SUCCEEDED(Listener.GetInitStatus());
    TEST_QUIC_SUCCEEDED(Listener.GetLocalAddr(ServerLocalAddr));

    for (uint64_t Bitmap = 1; Bitmap < 128; ++Bitmap) {
        char Name[64]; sprintf_s(Name, sizeof(Name), "DoHandshake %llu", (unsigned long long)Bitmap);
        TestScopeLogger logScope(Name);
        BitmapLossHelper LossHelper(Bitmap);
        MsQuicConnection Connection(Registration);
        TEST_QUIC_SUCCEEDED(Connection.GetInitStatus());
        TEST_QUIC_SUCCEEDED(Connection.Start(ClientConfiguration, ServerLocalAddr.GetFamily(), QUIC_TEST_LOOPBACK_FOR_AF(ServerLocalAddr.GetFamily()), ServerLocalAddr.GetPort()));
        TEST_TRUE(Connection.HandshakeCompleteEvent.WaitTimeout(TestWaitTimeout*20));
        TEST_TRUE(Connection.HandshakeComplete);
        Listener.LastConnection->Shutdown(0, QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT);
    }
}

struct ConnectionPoolConnectionContext {
    CxPlatEvent ConnectedEvent;
    uint16_t IdealProcessor;
    uint16_t PartitionIndex;
    bool Connected;

    static QUIC_STATUS ConnCallback(_In_ HQUIC, _In_opt_ void* Context, _Inout_ QUIC_CONNECTION_EVENT* Event) {
        auto* This = (ConnectionPoolConnectionContext*)Context;
        switch (Event->Type) {
            case QUIC_CONNECTION_EVENT_CONNECTED:
                This->Connected = true;
                This->ConnectedEvent.Set();
                break;
            case QUIC_CONNECTION_EVENT_IDEAL_PROCESSOR_CHANGED:
                This->IdealProcessor = Event->IDEAL_PROCESSOR_CHANGED.IdealProcessor;
                This->PartitionIndex = Event->IDEAL_PROCESSOR_CHANGED.PartitionIndex;
                break;
            default:
                break;
        }
        return QUIC_STATUS_SUCCESS;
    }
};

#ifdef QUIC_API_ENABLE_PREVIEW_FEATURES
void
QuicTestConnectionPoolCreate(
    _In_ int Family,
    _In_ uint16_t NumberOfConnections,
    _In_ bool XdpSupported,
    _In_ bool TestCibirSupport
    )
{
    QUIC_ADDRESS_FAMILY QuicAddrFamily = (Family == 4) ? QUIC_ADDRESS_FAMILY_INET : QUIC_ADDRESS_FAMILY_INET6;
    const uint8_t CibirId[] = { 0 /* offset */, 4, 3, 2, 1 };
    const uint8_t CibirIdLength = sizeof(CibirId);

    MsQuicRegistration Registration(true);
    TEST_QUIC_SUCCEEDED(Registration.GetInitStatus());
    MsQuicAlpn Alpn("MsQuicTest");

    MsQuicSettings Settings;
    Settings.SetIdleTimeoutMs(TestWaitTimeout);
    Settings.SetPeerBidiStreamCount(1);

    MsQuicConfiguration ServerConfiguration(Registration, Alpn, Settings, ServerSelfSignedCredConfig);
    TEST_QUIC_SUCCEEDED(ServerConfiguration.GetInitStatus());

    MsQuicConfiguration ClientConfiguration(Registration, Alpn, Settings, MsQuicCredentialConfig());
    TEST_QUIC_SUCCEEDED(ClientConfiguration.GetInitStatus());

    QuicAddr ServerAddr(QuicAddrFamily);
    if (XdpSupported) {
        QuicAddrSetToDuoNic(&ServerAddr.SockAddr);
    }

    {
        UniquePtrArray<ConnectionScope> Connections(new(std::nothrow) ConnectionScope[NumberOfConnections]);
        TEST_NOT_EQUAL(nullptr, Connections);

        UniquePtrArray<ConnectionPoolConnectionContext> Contexts(new(std::nothrow) ConnectionPoolConnectionContext[NumberOfConnections]);
        TEST_NOT_EQUAL(nullptr, Contexts);

        UniquePtrArray<ConnectionPoolConnectionContext*> ContextPtrs(new(std::nothrow) ConnectionPoolConnectionContext*[NumberOfConnections]);
        TEST_NOT_EQUAL(nullptr, ContextPtrs);

        for(uint32_t i = 0; i < NumberOfConnections; ++i) {
            ContextPtrs[i] = &Contexts[i];
            Contexts[i].Connected = false;
            Contexts[i].IdealProcessor = 0;
            Contexts[i].PartitionIndex = 0;
        }

        MsQuicAutoAcceptListener Listener(Registration, ServerConfiguration, MsQuicConnection::NoOpCallback);
        TEST_QUIC_SUCCEEDED(Listener.GetInitStatus());

        if (TestCibirSupport) {
            TEST_QUIC_SUCCEEDED(Listener.SetCibirId(CibirId, CibirIdLength));
        }

        TEST_QUIC_SUCCEEDED(Listener.Start(Alpn, ServerAddr));
        TEST_QUIC_SUCCEEDED(Listener.GetLocalAddr(ServerAddr));

        {
            UniquePtrArray<uint8_t*> CibirIdPtrs(new(std::nothrow) uint8_t*[NumberOfConnections]);
            TEST_NOT_EQUAL(nullptr, CibirIdPtrs);
            UniquePtrArray<uint8_t> CibirIdBuffer(new(std::nothrow) uint8_t[NumberOfConnections * CibirIdLength]);
            TEST_NOT_EQUAL(nullptr, CibirIdBuffer);
            QUIC_CONNECTION_POOL_CONFIG PoolConfig{};
            PoolConfig.Registration = Registration;
            PoolConfig.Configuration = ClientConfiguration;
            PoolConfig.Handler = ConnectionPoolConnectionContext::ConnCallback;
            PoolConfig.ServerName = QUIC_LOCALHOST_FOR_AF(QuicAddrFamily);
            if (XdpSupported) {
                PoolConfig.ServerAddress = &ServerAddr.SockAddr;
            }
            PoolConfig.ServerPort = ServerAddr.GetPort();
            PoolConfig.Context = (void**)ContextPtrs.get();
            PoolConfig.Family = QuicAddrFamily;
            PoolConfig.NumberOfConnections = NumberOfConnections;
            PoolConfig.Flags = QUIC_CONNECTION_POOL_FLAG_NONE;

            if (TestCibirSupport) {
                for (uint32_t i = 0; i < NumberOfConnections; i++) {
                    CxPlatCopyMemory(&(CibirIdBuffer[i * CibirIdLength]), CibirId, CibirIdLength);
                    CibirIdPtrs[i] = &CibirIdBuffer[i * CibirIdLength];
                }
                PoolConfig.CibirIds = CibirIdPtrs.get();
                PoolConfig.CibirIdLength = CibirIdLength;
            }

            QUIC_STATUS Status = MsQuic->ConnectionPoolCreate(&PoolConfig, &(Connections.get()->Handle));
            if (XdpSupported) {
                TEST_QUIC_SUCCEEDED(Status);
                for (uint32_t i = 0; i < NumberOfConnections; i++) {
                    //
                    // Verify the client connection is connected.
                    //
                    Contexts[i].ConnectedEvent.WaitTimeout(TestWaitTimeout);
                    if (!Contexts[i].Connected) {
                        TEST_FAILURE("Client connection %u failed to connect", i);
                    }
                }
                TEST_EQUAL(NumberOfConnections, Listener.AcceptedConnectionCount);
            } else {
                //
                // When testing no XDP support, the loopback address is used,
                // and XDP doesn't support the loopback interface, so the
                // expected error is "NOT_FOUND" since XDP isn't found for that
                // interface.
                // In the case the platform doesn't support XDP at all,
                // NOT_SUPPORTED is expected.
                //
                if (Status != QUIC_STATUS_NOT_SUPPORTED &&
                    Status != QUIC_STATUS_FILE_NOT_FOUND &&
                    Status != QUIC_STATUS_NOT_FOUND) {
                    TEST_FAILURE(
                        "Expected QUIC_STATUS_NOT_SUPPORTED or QUIC_STATUS_NOT_FOUND, but got 0x%x",
                        Status);
                }
            }
        }
    }
}
#endif // QUIC_API_ENABLE_PREVIEW_FEATURES
