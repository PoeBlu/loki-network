#ifndef LLARP_LINK_UTP_INTERNAL_HPP
#define LLARP_LINK_UTP_INTERNAL_HPP

#include <constants/link_layer.hpp>
#include <crypto/types.hpp>
#include <link/utp.hpp>
#include <util/aligned.hpp>
#include <utp.h>

#include <tuple>
#include <deque>

namespace llarp
{
  struct Crypto;
  namespace utp
  {
    /// size of keyed hash
    constexpr size_t FragmentHashSize = 32;
    /// size of outer nonce
    constexpr size_t FragmentNonceSize = 32;
    /// size of outer overhead
    constexpr size_t FragmentOverheadSize =
        FragmentHashSize + FragmentNonceSize;
    /// max fragment payload size
    constexpr size_t FragmentBodyPayloadSize = 512;
    /// size of inner nonce
    constexpr size_t FragmentBodyNonceSize = 24;
    /// size of fragment body overhead
    constexpr size_t FragmentBodyOverhead = FragmentBodyNonceSize
        + sizeof(uint32) + sizeof(uint16_t) + sizeof(uint16_t);
    /// size of fragment body
    constexpr size_t FragmentBodySize =
        FragmentBodyOverhead + FragmentBodyPayloadSize;
    /// size of fragment
    constexpr size_t FragmentBufferSize =
        FragmentOverheadSize + FragmentBodySize;

    static_assert(FragmentBufferSize == 608, "Fragment Buffer Size is not 608");

    /// buffer for a single utp fragment
    using FragmentBuffer = llarp::AlignedBuffer< FragmentBufferSize >;

    /// maximum size for send queue for a session before we drop
    constexpr size_t MaxSendQueueSize = 64;

    /// buffer for a link layer message
    using MessageBuffer = llarp::AlignedBuffer< MAX_LINK_MSG_SIZE >;

    struct LinkLayer;

    /// pending inbound message being received
    struct InboundMessage
    {
      /// timestamp of last activity
      llarp_time_t lastActive;
      /// the underlying message buffer
      MessageBuffer _msg;

      /// for accessing message buffer
      llarp_buffer_t buffer;

      InboundMessage() : lastActive(0), _msg(), buffer(_msg)
      {
      }

      InboundMessage(const InboundMessage& other)
          : lastActive(other.lastActive), _msg(other._msg), buffer(_msg)
      {
        buffer.cur = buffer.base + (other.buffer.cur - other.buffer.base);
        buffer.sz  = other.buffer.sz;
      }

      bool
      operator==(const InboundMessage& other) const
      {
        return buffer.base == other.buffer.base;
      }

      /// return true if this inbound message can be removed due to expiration
      bool
      IsExpired(llarp_time_t now) const;

      /// append data at ptr of size sz bytes to message buffer
      /// increment current position
      /// return false if we don't have enough room
      /// return true on success
      bool
      AppendData(const byte_t* ptr, uint16_t sz);
    };

    struct Session final : public ILinkSession
    {
      /// remote router's rc
      RouterContact remoteRC;
      /// underlying socket
      utp_socket* sock;
      /// link layer parent
      LinkLayer* parent;
      /// did we get a LIM from the remote yet?
      bool gotLIM;
      /// remote router's transport pubkey
      PubKey remoteTransportPubKey;
      /// remote router's transport ip
      Addr remoteAddr;
      /// rx session key
      SharedSecret rxKey;
      /// tx session key
      SharedSecret txKey;
      /// timestamp last active
      llarp_time_t lastActive;
      /// session timeout (60s)
      const static llarp_time_t sessionTimeout = DefaultLinkSessionLifetime;

      /// send queue for utp
      std::deque< utp_iovec > vecq;
      /// tx fragment queue
      std::deque< FragmentBuffer > sendq;
      /// current rx fragment buffer
      FragmentBuffer recvBuf;
      /// current offset in current rx fragment buffer
      size_t recvBufOffset;
      /// rx fragment message body
      AlignedBuffer< FragmentBodySize > rxFragBody;

      /// the next message id for tx
      uint32_t m_NextTXMsgID;
      /// the next message id for rx
      uint32_t m_NextRXMsgID;
      /// messages we are recving right now
      std::unordered_map< uint32_t, InboundMessage > m_RecvMsgs;
      /// are we stalled or nah?
      bool stalled = false;

      uint64_t m_RXRate = 0;
      uint64_t m_TXRate = 0;

      /// mark session as alive
      void
      Alive();

      util::StatusObject
      ExtractStatus() const override;

      /// base
      Session(LinkLayer* p);

      /// outbound
      Session(LinkLayer* p, utp_socket* s, const RouterContact& rc,
              const AddressInfo& addr);

      /// inbound
      Session(LinkLayer* p, utp_socket* s, const Addr& remote);

      enum State
      {
        eInitial,          // initial state
        eConnecting,       // we are connecting
        eLinkEstablished,  // when utp connection is established
        eCryptoHandshake,  // crypto handshake initiated
        eSessionReady,     // session is ready
        eClose             // utp connection is closed
      };

      /// get router
      // llarp::Router*
      // Router();

      llarp::Crypto*
      OurCrypto();

      /// session state, call EnterState(State) to set
      State state;

      /// hook for utp for when we have established a connection
      void
      OnLinkEstablished(LinkLayer* p);

      /// switch states
      void
      EnterState(State st);

      Session();
      ~Session();

      /// handle LIM after handshake
      bool
      GotSessionRenegotiate(const LinkIntroMessage* msg);

      /// re negotiate session with our new local RC
      bool
      Rehandshake();

      /// pump tx queue
      void
      PumpWrite();

      void
      DoPump();

      /// verify a fragment buffer and the decrypt it
      /// buf is assumed to be FragmentBufferSize bytes long
      bool
      VerifyThenDecrypt(const byte_t* buf);

      /// encrypt a fragment then hash the ciphertext
      bool
      EncryptThenHash(const byte_t* ptr, uint32_t msgid, uint16_t sz,
                      uint16_t remain);

      /// queue a fully formed message
      bool
      QueueWriteBuffers(const llarp_buffer_t& buf);

      /// prune expired inbound messages
      void
      PruneInboundMessages(llarp_time_t now);

      /// do low level connect
      void
      Connect();

      /// handle outbound connection made
      void
      OutboundLinkEstablished(LinkLayer* p);

      // send first message
      void
      OutboundHandshake();

      // do key exchange for handshake
      bool
      DoKeyExchange(transport_dh_func dh, SharedSecret& K,
                    const KeyExchangeNonce& n, const PubKey& other,
                    const SecretKey& secret);

      /// does K = HS(K + A)
      bool
      MutateKey(SharedSecret& K, const AlignedBuffer< 24 >& A);

      void
      TickImpl(llarp_time_t now);

      /// close session
      void
      Close();

      /// low level read
      bool
      Recv(const byte_t* buf, size_t sz);

      /// handle inbound LIM
      bool
      InboundLIM(const LinkIntroMessage* msg);

      /// handle outbound LIM
      bool
      OutboundLIM(const LinkIntroMessage* msg);

      /// return true if timed out
      bool
      IsTimedOut(llarp_time_t now) const;

      /// get remote identity pubkey
      const PubKey&
      RemotePubKey() const;

      /// get remote address
      Addr
      RemoteEndpoint();

      /// get parent link
      ILinkLayer*
      GetParent();

      void
      MarkEstablished();
    };

    struct LinkLayer final : public ILinkLayer
    {
      utp_context* _utp_ctx  = nullptr;
      llarp::Crypto* _crypto = nullptr;

      // low level read callback
      static uint64
      OnRead(utp_callback_arguments* arg);

      // low level sendto callback
      static uint64
      SendTo(utp_callback_arguments* arg);

      /// error callback
      static uint64
      OnError(utp_callback_arguments* arg);

      /// state change callback
      static uint64
      OnStateChange(utp_callback_arguments*);

      static uint64
      OnConnect(utp_callback_arguments*);

      /// accept callback
      static uint64
      OnAccept(utp_callback_arguments*);

      /// logger callback
      static uint64
      OnLog(utp_callback_arguments* arg);

      /// construct
      LinkLayer(llarp::Crypto* crypto, const SecretKey& routerEncSecret,
                llarp::GetRCFunc getrc, llarp::LinkMessageHandler h,
                llarp::SignBufferFunc sign,
                llarp::SessionEstablishedHandler established,
                llarp::SessionRenegotiateHandler reneg,
                llarp::TimeoutHandler timeout,
                llarp::SessionClosedHandler closed);

      /// destruct
      ~LinkLayer();

      /// get AI rank
      uint16_t
      Rank() const;

      /// handle low level recv
      void
      RecvFrom(const Addr& from, const void* buf, size_t sz);

#ifdef __linux__
      /// process ICMP stuff on linux
      void
      ProcessICMP();
#endif

      llarp::Crypto*
      OurCrypto();

      /// pump sessions
      void
      Pump();

      /// stop link layer
      void
      Stop();

      /// regenerate transport keypair
      bool
      KeyGen(SecretKey& k);

      /// do tick
      void
      Tick(llarp_time_t now);

      /// create new outbound session
      ILinkSession*
      NewOutboundSession(const RouterContact& rc, const AddressInfo& addr);

      /// create new socket
      utp_socket*
      NewSocket();

      /// get ai name
      const char*
      Name() const;
    };

  }  // namespace utp
}  // namespace llarp

#endif
