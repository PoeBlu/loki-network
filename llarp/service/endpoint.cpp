
#include <llarp/dht/messages/findintro.hpp>
#include <llarp/messages/dht.hpp>
#include <llarp/service/endpoint.hpp>
#include <llarp/service/protocol.hpp>
#include "buffer.hpp"
#include "router.hpp"

namespace llarp
{
  namespace service
  {
    Endpoint::Endpoint(const std::string& name, llarp_router* r)
        : llarp_pathbuilder_context(r, r->dht, 2, 4), m_Router(r), m_Name(name)
    {
      m_Tag.Zero();
    }

    bool
    Endpoint::SetOption(const std::string& k, const std::string& v)
    {
      if(k == "keyfile")
      {
        m_Keyfile = v;
      }
      if(k == "tag")
      {
        m_Tag = v;
        llarp::LogInfo("Setting tag to ", v);
      }
      if(k == "prefetch-tag")
      {
        m_PrefetchTags.insert(v);
      }
      if(k == "prefetch-addr")
      {
        Address addr;
        if(addr.FromString(v))
          m_PrefetchAddrs.insert(addr);
      }
      if(k == "netns")
      {
        m_NetNS = v;
        m_OnInit.push_back(std::bind(&Endpoint::IsolateNetwork, this));
      }
      return true;
    }

    bool
    Endpoint::IsolateNetwork()
    {
      m_IsolatedWorker = llarp_init_isolated_net_threadpool(
          m_Name.c_str(), &SetupIsolatedNetwork, this);
      m_IsolatedLogic = llarp_init_single_process_logic(m_IsolatedWorker);
      return true;
    }

    struct PathAlignJob
    {
      void
      HandleResult(Endpoint::OutboundContext* context)
      {
        if(context)
        {
          byte_t tmp[128] = {0};
          memcpy(tmp, "BEEP", 4);
          auto buf = llarp::StackBuffer< decltype(tmp) >(tmp);
          buf.sz   = 4;
          context->AsyncEncryptAndSendTo(buf, eProtocolText);
        }
        else
        {
          llarp::LogWarn("PathAlignJob timed out");
        }
        delete this;
      }
    };

    bool
    Endpoint::SetupIsolatedNetwork(void* user)
    {
      return static_cast< Endpoint* >(user)->DoNetworkIsolation();
    }

    bool
    Endpoint::HasPendingPathToService(const Address& addr) const
    {
      return m_PendingServiceLookups.find(addr)
          != m_PendingServiceLookups.end();
    }

    void
    Endpoint::Tick(llarp_time_t now)
    {
      /// reset tx id for publish
      if(now - m_LastPublishAttempt >= INTROSET_PUBLISH_RETRY_INTERVAL)
        m_CurrentPublishTX = 0;
      // publish descriptors
      if(ShouldPublishDescriptors(now))
      {
        std::set< Introduction > I;
        if(!GetCurrentIntroductions(I))
        {
          llarp::LogWarn("could not publish descriptors for endpoint ", Name(),
                         " because we couldn't get any introductions");
          if(ShouldBuildMore())
            ManualRebuild(1);
          return;
        }
        m_IntroSet.I.clear();
        for(const auto& intro : I)
          m_IntroSet.I.push_back(intro);
        m_IntroSet.topic = m_Tag;
        if(!m_Identity.SignIntroSet(m_IntroSet, &m_Router->crypto))
        {
          llarp::LogWarn("failed to sign introset for endpoint ", Name());
          return;
        }
        if(PublishIntroSet(m_Router))
        {
          llarp::LogInfo("publishing introset for endpoint ", Name());
        }
        else
        {
          llarp::LogWarn("failed to publish intro set for endpoint ", Name());
        }
      }
      // expire pending tx
      {
        std::set< service::IntroSet > empty;
        auto itr = m_PendingLookups.begin();
        while(itr != m_PendingLookups.end())
        {
          if(itr->second->IsTimedOut(now))
          {
            std::unique_ptr< IServiceLookup > lookup = std::move(itr->second);

            llarp::LogInfo(lookup->name, " timed out txid=", lookup->txid);
            lookup->HandleResponse(empty);
            itr = m_PendingLookups.erase(itr);
          }
          else
            ++itr;
        }
      }

      // expire pending router lookups
      {
        auto itr = m_PendingRouters.begin();
        while(itr != m_PendingRouters.end())
        {
          if(itr->second.IsExpired(now))
          {
            llarp::LogInfo("lookup for ", itr->first, " timed out");
            itr = m_PendingRouters.erase(itr);
          }
          else
            ++itr;
        }
      }

      // prefetch addrs
      for(const auto& addr : m_PrefetchAddrs)
      {
        if(!HasPathToService(addr))
        {
          PathAlignJob* j = new PathAlignJob();
          if(!EnsurePathToService(addr,
                                  std::bind(&PathAlignJob::HandleResult, j,
                                            std::placeholders::_1),
                                  10000))
          {
            llarp::LogWarn("failed to ensure path to ", addr);
            delete j;
          }
        }
      }

      // prefetch tags
      for(const auto& tag : m_PrefetchTags)
      {
        auto itr = m_PrefetchedTags.find(tag);
        if(itr == m_PrefetchedTags.end())
        {
          itr =
              m_PrefetchedTags.insert(std::make_pair(tag, CachedTagResult(tag)))
                  .first;
        }
        for(const auto& introset : itr->second.result)
        {
          if(HasPendingPathToService(introset.A.Addr()))
            continue;
          PathAlignJob* j = new PathAlignJob();
          if(!EnsurePathToService(introset.A.Addr(),
                                  std::bind(&PathAlignJob::HandleResult, j,
                                            std::placeholders::_1),
                                  10000))
          {
            llarp::LogWarn("failed to ensure path to ", introset.A.Addr(),
                           " for tag ", tag.ToString());
            delete j;
          }
        }
        itr->second.Expire(now);
        if(itr->second.ShouldRefresh(now))
        {
          auto path = PickRandomEstablishedPath();
          if(path)
          {
            auto job = new TagLookupJob(this, &itr->second);
            job->SendRequestViaPath(path, Router());
          }
        }
      }

      // tick remote sessions
      {
        auto itr = m_RemoteSessions.begin();
        while(itr != m_RemoteSessions.end())
        {
          if(itr->second->Tick(now))
          {
            delete itr->second;
            itr = m_RemoteSessions.erase(itr);
          }
          else
            ++itr;
        }
      }
    }

    uint64_t
    Endpoint::GenTXID()
    {
      uint64_t txid = llarp_randint();
      while(m_PendingLookups.find(txid) != m_PendingLookups.end())
        ++txid;
      return txid;
    }

    std::string
    Endpoint::Name() const
    {
      return m_Name + ":" + m_Identity.pub.Name();
    }

    bool
    Endpoint::HasPathToService(const Address& addr) const
    {
      return m_RemoteSessions.find(addr) != m_RemoteSessions.end();
    }

    void
    Endpoint::PutLookup(IServiceLookup* lookup, uint64_t txid)
    {
      m_PendingLookups.insert(std::make_pair(txid, lookup));
    }

    bool
    Endpoint::HandleGotIntroMessage(const llarp::dht::GotIntroMessage* msg)
    {
      auto crypto = &m_Router->crypto;
      std::set< IntroSet > remote;
      for(const auto& introset : msg->I)
      {
        if(!introset.VerifySignature(crypto))
        {
          llarp::LogInfo("invalid introset signature for ", introset,
                         " on endpoint ", Name());
          if(m_Identity.pub == introset.A && m_CurrentPublishTX == msg->T)
          {
            IntroSetPublishFail();
          }
          return false;
        }
        if(m_Identity.pub == introset.A && m_CurrentPublishTX == msg->T)
        {
          llarp::LogInfo(
              "got introset publish confirmation for hidden service endpoint ",
              Name());
          IntroSetPublished();
          return true;
        }
        else
        {
          remote.insert(introset);
        }
      }
      auto itr = m_PendingLookups.find(msg->T);
      if(itr == m_PendingLookups.end())
      {
        llarp::LogWarn("invalid lookup response for hidden service endpoint ",
                       Name(), " txid=", msg->T);
        return true;
      }
      std::unique_ptr< IServiceLookup > lookup = std::move(itr->second);
      m_PendingLookups.erase(itr);
      lookup->HandleResponse(remote);
      return true;
    }

    void
    Endpoint::PutSenderFor(const ConvoTag& tag, const ServiceInfo& info)
    {
      auto itr = m_Sessions.find(tag);
      if(itr == m_Sessions.end())
      {
        itr = m_Sessions.insert(std::make_pair(tag, Session{})).first;
      }
      itr->second.remote   = info;
      itr->second.lastUsed = llarp_time_now_ms();
    }

    bool
    Endpoint::GetSenderFor(const ConvoTag& tag, ServiceInfo& si) const
    {
      auto itr = m_Sessions.find(tag);
      if(itr == m_Sessions.end())
        return false;
      si = itr->second.remote;
      return true;
    }

    void
    Endpoint::PutIntroFor(const ConvoTag& tag, const Introduction& intro)
    {
      auto itr = m_Sessions.find(tag);
      if(itr == m_Sessions.end())
      {
        itr = m_Sessions.insert(std::make_pair(tag, Session{})).first;
      }
      itr->second.intro    = intro;
      itr->second.lastUsed = llarp_time_now_ms();
    }

    bool
    Endpoint::GetIntroFor(const ConvoTag& tag, Introduction& intro) const
    {
      auto itr = m_Sessions.find(tag);
      if(itr == m_Sessions.end())
        return false;
      intro = itr->second.intro;
      return true;
    }

    bool
    Endpoint::GetConvoTagsForService(const ServiceInfo& info,
                                     std::set< ConvoTag >& tags) const
    {
      bool inserted = false;
      auto itr      = m_Sessions.begin();
      while(itr != m_Sessions.end())
      {
        if(itr->second.remote == info)
        {
          inserted |= tags.insert(itr->first).second;
        }
      }
      return inserted;
    }

    bool
    Endpoint::GetCachedSessionKeyFor(const ConvoTag& tag,
                                     const byte_t*& secret) const
    {
      auto itr = m_Sessions.find(tag);
      if(itr == m_Sessions.end())
        return false;
      secret = itr->second.sharedKey.data();
      return true;
    }

    void
    Endpoint::PutCachedSessionKeyFor(const ConvoTag& tag, const SharedSecret& k)
    {
      auto itr = m_Sessions.find(tag);
      if(itr == m_Sessions.end())
      {
        itr = m_Sessions.insert(std::make_pair(tag, Session{})).first;
      }
      itr->second.sharedKey = k;
      itr->second.lastUsed  = llarp_time_now_ms();
    }

    bool
    Endpoint::Start()
    {
      auto crypto = &m_Router->crypto;
      if(m_Keyfile.size())
      {
        if(!m_Identity.EnsureKeys(m_Keyfile, crypto))
          return false;
      }
      else
      {
        m_Identity.RegenerateKeys(crypto);
      }
      if(!m_DataHandler)
      {
        m_DataHandler = this;
      }
      while(m_OnInit.size())
      {
        if(m_OnInit.front()())
          m_OnInit.pop_front();
        else
          return false;
      }
      return true;
    }

    Endpoint::~Endpoint()
    {
    }
    bool
    Endpoint::CachedTagResult::HandleResponse(
        const std::set< IntroSet >& introsets)
    {
      auto now = llarp_time_now_ms();

      for(const auto& introset : introsets)
        if(result.insert(introset).second)
          lastModified = now;
      llarp::LogInfo("Tag result for ", tag.ToString(), " got ",
                     introsets.size(), " results from lookup, have ",
                     result.size(), " cached last modified at ", lastModified,
                     " is ", now - lastModified, "ms old");
      return true;
    }

    void
    Endpoint::CachedTagResult::Expire(llarp_time_t now)
    {
      auto itr = result.begin();
      while(itr != result.end())
      {
        if(itr->HasExpiredIntros(now))
        {
          llarp::LogInfo("Removing expired tag Entry ", itr->A.Name());
          itr          = result.erase(itr);
          lastModified = now;
        }
        else
        {
          ++itr;
        }
      }
    }

    llarp::routing::IMessage*
    Endpoint::CachedTagResult::BuildRequestMessage(uint64_t txid)
    {
      llarp::routing::DHTMessage* msg = new llarp::routing::DHTMessage();
      msg->M.push_back(new llarp::dht::FindIntroMessage(tag, txid));
      lastRequest = llarp_time_now_ms();
      return msg;
    }

    bool
    Endpoint::PublishIntroSet(llarp_router* r)
    {
      auto path = GetEstablishedPathClosestTo(m_Identity.pub.Addr().ToRouter());
      if(path)
      {
        m_CurrentPublishTX = llarp_randint();
        llarp::routing::DHTMessage msg;
        msg.M.push_back(new llarp::dht::PublishIntroMessage(
            m_IntroSet, m_CurrentPublishTX, 4));
        if(path->SendRoutingMessage(&msg, r))
        {
          m_LastPublishAttempt = llarp_time_now_ms();
          llarp::LogInfo(Name(), " publishing introset");
          return true;
        }
      }
      llarp::LogWarn(Name(), " publish introset failed, no path");
      return false;
    }

    void
    Endpoint::IntroSetPublishFail()
    {
      llarp::LogWarn("failed to publish introset for ", Name());
      m_CurrentPublishTX = 0;
    }

    bool
    Endpoint::ShouldPublishDescriptors(llarp_time_t now) const
    {
      if(m_IntroSet.HasExpiredIntros(now))
        return m_CurrentPublishTX == 0
            && now - m_LastPublishAttempt >= INTROSET_PUBLISH_RETRY_INTERVAL;
      return m_CurrentPublishTX == 0
          && now - m_LastPublish >= INTROSET_PUBLISH_INTERVAL;
    }

    void
    Endpoint::IntroSetPublished()
    {
      m_CurrentPublishTX = 0;
      m_LastPublish      = llarp_time_now_ms();
      llarp::LogInfo(Name(), " IntroSet publish confirmed");
    }

    struct HiddenServiceAddressLookup : public IServiceLookup
    {
      ~HiddenServiceAddressLookup()
      {
      }

      Address remote;
      typedef std::function< bool(const IntroSet*) > HandlerFunc;
      HandlerFunc handle;

      HiddenServiceAddressLookup(Endpoint* p, HandlerFunc h,
                                 const Address& addr, uint64_t tx)
          : IServiceLookup(p, tx, "HSLookup"), remote(addr), handle(h)
      {
      }

      bool
      HandleResponse(const std::set< IntroSet >& results)
      {
        llarp::LogInfo("found ", results.size(), " for ", remote.ToString());
        if(results.size() == 1)
        {
          llarp::LogInfo("hidden service lookup for ", remote.ToString(),
                         " success");
          handle(&*results.begin());
        }
        else
        {
          llarp::LogInfo("no response in hidden service lookup for ",
                         remote.ToString());
          handle(nullptr);
        }
        return false;
      }

      llarp::routing::IMessage*
      BuildRequestMessage()
      {
        llarp::routing::DHTMessage* msg = new llarp::routing::DHTMessage();
        auto FIM = new llarp::dht::FindIntroMessage(txid, remote);
        FIM->R   = 5;
        msg->M.push_back(FIM);
        llarp::LogInfo("build request for ", remote);
        return msg;
      }
    };

    bool
    Endpoint::DoNetworkIsolation()
    {
      /// TODO: implement me
      return false;
    }

    void
    Endpoint::PutNewOutboundContext(const llarp::service::IntroSet& introset)
    {
      Address addr;
      introset.A.CalculateAddress(addr.data());

      // only add new session if it's not there
      if(m_RemoteSessions.find(addr) == m_RemoteSessions.end())
      {
        OutboundContext* ctx = new OutboundContext(introset, this);
        m_RemoteSessions.insert(std::make_pair(addr, ctx));
        llarp::LogInfo("Created New outbound context for ", addr.ToString());
      }

      // inform pending
      auto itr = m_PendingServiceLookups.find(addr);
      if(itr != m_PendingServiceLookups.end())
      {
        auto f = itr->second;
        m_PendingServiceLookups.erase(itr);
        f(m_RemoteSessions.at(addr));
      }
    }

    bool
    Endpoint::HandleGotRouterMessage(const llarp::dht::GotRouterMessage* msg)
    {
      bool success = false;
      if(msg->R.size() == 1)
      {
        auto itr = m_PendingRouters.find(msg->R[0].pubkey);
        if(itr == m_PendingRouters.end())
          return false;
        llarp_async_verify_rc* job = new llarp_async_verify_rc;
        job->nodedb                = m_Router->nodedb;
        job->cryptoworker          = m_Router->tp;
        job->diskworker            = m_Router->disk;
        job->logic                 = nullptr;
        job->hook                  = nullptr;
        llarp_rc_clear(&job->rc);
        llarp_rc_copy(&job->rc, &msg->R[0]);
        llarp_nodedb_async_verify(job);
        return true;
      }
      return success;
    }

    void
    Endpoint::EnsureRouterIsKnown(const RouterID& router)
    {
      if(router.IsZero())
        return;
      if(!llarp_nodedb_get_rc(m_Router->nodedb, router))
      {
        if(m_PendingRouters.find(router) == m_PendingRouters.end())
        {
          auto path = GetEstablishedPathClosestTo(router);
          routing::DHTMessage msg;
          auto txid = GenTXID();
          msg.M.push_back(
              new dht::FindRouterMessage({}, dht::Key_t(router), txid));

          if(path && path->SendRoutingMessage(&msg, m_Router))
          {
            llarp::LogInfo(Name(), " looking up ", router);
            m_PendingRouters.insert(
                std::make_pair(router, RouterLookupJob(this)));
          }
          else
          {
            llarp::LogError("failed to send request for router lookup");
          }
        }
      }
    }

    void
    Endpoint::HandlePathBuilt(path::Path* p)
    {
      p->SetDataHandler(std::bind(&Endpoint::HandleHiddenServiceFrame, this,
                                  std::placeholders::_1));
    }

    bool
    Endpoint::HandleHiddenServiceFrame(const ProtocolFrame* frame)
    {
      return frame->AsyncDecryptAndVerify(EndpointLogic(), Crypto(), Worker(),
                                          m_Identity, m_DataHandler);
    }

    void
    Endpoint::OutboundContext::HandlePathBuilt(path::Path* p)
    {
      p->SetDataHandler(
          std::bind(&Endpoint::OutboundContext::HandleHiddenServiceFrame, this,
                    std::placeholders::_1));
    }

    bool
    Endpoint::OutboundContext::HandleHiddenServiceFrame(
        const ProtocolFrame* frame)
    {
      return m_Parent->HandleHiddenServiceFrame(frame);
    }

    bool
    Endpoint::OnOutboundLookup(const IntroSet* introset)
    {
      if(!introset)
        return false;
      PutNewOutboundContext(*introset);
      return true;
    }

    bool
    Endpoint::EnsurePathToService(const Address& remote, PathEnsureHook hook,
                                  llarp_time_t timeoutMS)
    {
      auto path = GetEstablishedPathClosestTo(remote.ToRouter());
      if(!path)
      {
        llarp::LogWarn("No outbound path for lookup yet");
        return false;
      }
      llarp::LogInfo(Name(), " Ensure Path to ", remote.ToString());
      {
        auto itr = m_RemoteSessions.find(remote);
        if(itr != m_RemoteSessions.end())
        {
          hook(itr->second);
          return true;
        }
      }
      auto itr = m_PendingServiceLookups.find(remote);
      if(itr != m_PendingServiceLookups.end())
      {
        // duplicate
        llarp::LogWarn("duplicate pending service lookup to ",
                       remote.ToString());
        return false;
      }

      m_PendingServiceLookups.insert(std::make_pair(remote, hook));

      HiddenServiceAddressLookup* job = new HiddenServiceAddressLookup(
          this,
          std::bind(&Endpoint::OnOutboundLookup, this, std::placeholders::_1),
          remote, GenTXID());

      if(job->SendRequestViaPath(path, Router()))
        return true;
      llarp::LogError("send via path failed");
      return false;
    }

    Endpoint::OutboundContext::OutboundContext(const IntroSet& intro,
                                               Endpoint* parent)
        : llarp_pathbuilder_context(parent->m_Router, parent->m_Router->dht, 2,
                                    4)
        , currentIntroSet(intro)
        , m_Parent(parent)

    {
      selectedIntro.Clear();
      ShiftIntroduction();
    }

    Endpoint::OutboundContext::~OutboundContext()
    {
    }

    bool
    Endpoint::OutboundContext::OnIntroSetUpdate(const IntroSet* i)
    {
      if(i && i->IsNewerThan(currentIntroSet))
      {
        currentIntroSet = *i;
      }
      return true;
    }

    void
    Endpoint::OutboundContext::ShiftIntroduction()
    {
      for(const auto& intro : currentIntroSet.I)
      {
        m_Parent->EnsureRouterIsKnown(selectedIntro.router);
        if(intro.expiresAt > selectedIntro.expiresAt)
        {
          selectedIntro = intro;
        }
      }
      ManualRebuild(2);
    }

    void
    Endpoint::OutboundContext::AsyncEncryptAndSendTo(llarp_buffer_t data,
                                                     ProtocolType protocol)
    {
      auto path = GetPathByRouter(selectedIntro.router);
      if(!path)
      {
        llarp::LogError("No Path to ", selectedIntro.router, " yet");
        return;
      }
      if(sequenceNo)
      {
        EncryptAndSendTo(path, data, protocol);
      }
      else
      {
        AsyncGenIntro(path, data, protocol);
      }
    }

    struct AsyncIntroGen
    {
      llarp_logic* logic;
      llarp_crypto* crypto;
      byte_t* sharedKey;
      ServiceInfo remote;
      const Identity& m_LocalIdentity;
      ProtocolMessage msg;
      ProtocolFrame frame;
      Introduction intro;
      const PQPubKey introPubKey;
      std::function< void(ProtocolFrame&) > hook;
      IDataHandler* handler;

      AsyncIntroGen(llarp_logic* l, llarp_crypto* c, byte_t* key,
                    const ServiceInfo& r, const Identity& localident,
                    const PQPubKey& introsetPubKey, const Introduction& us,
                    IDataHandler* h)
          : logic(l)
          , crypto(c)
          , sharedKey(key)
          , remote(r)
          , m_LocalIdentity(localident)
          , intro(us)
          , introPubKey(introsetPubKey)
          , handler(h)
      {
      }

      static void
      Result(void* user)
      {
        AsyncIntroGen* self = static_cast< AsyncIntroGen* >(user);
        // put values
        self->handler->PutCachedSessionKeyFor(self->msg.tag, self->sharedKey);
        self->handler->PutIntroFor(self->msg.tag, self->msg.introReply);
        self->handler->PutSenderFor(self->msg.tag, self->remote);
        self->hook(self->frame);
        delete self;
      }

      static void
      Work(void* user)
      {
        AsyncIntroGen* self = static_cast< AsyncIntroGen* >(user);
        // derive ntru session key component
        SharedSecret K;
        self->crypto->pqe_encrypt(self->frame.C, K, self->introPubKey);
        // randomize Nounce
        self->frame.N.Randomize();
        // compure post handshake session key
        byte_t tmp[64];
        // K
        memcpy(tmp, K, 32);
        // PKE (A, B, N)
        if(!self->m_LocalIdentity.KeyExchange(self->crypto->dh_client, tmp + 32,
                                              self->remote, self->frame.N))
          llarp::LogError("failed to derive x25519 shared key component");
        // H (K + PKE(A, B, N))
        self->crypto->shorthash(self->sharedKey,
                                llarp::StackBuffer< decltype(tmp) >(tmp));
        // randomize tag
        self->msg.tag.Randomize();
        // set sender
        self->msg.sender = self->m_LocalIdentity.pub;
        // set our introduction
        self->msg.introReply = self->intro;
        // encrypt and sign
        if(self->frame.EncryptAndSign(self->crypto, self->msg, K,
                                      self->m_LocalIdentity))
          llarp_logic_queue_job(self->logic, {self, &Result});
        else
          llarp::LogError("failed to encrypt and sign");
      }
    };

    void
    Endpoint::OutboundContext::AsyncGenIntro(path::Path* p,
                                             llarp_buffer_t payload,
                                             ProtocolType t)
    {
      AsyncIntroGen* ex = new AsyncIntroGen(
          m_Parent->RouterLogic(), m_Parent->Crypto(), sharedKey,
          currentIntroSet.A, m_Parent->GetIdentity(), currentIntroSet.K,
          selectedIntro, m_Parent->m_DataHandler);
      ex->hook = std::bind(&Endpoint::OutboundContext::Send, this,
                           std::placeholders::_1);
      ex->msg.PutBuffer(payload);
      ex->msg.introReply = p->intro;
      llarp_threadpool_queue_job(m_Parent->Worker(),
                                 {ex, &AsyncIntroGen::Work});
    }

    void
    Endpoint::OutboundContext::Send(ProtocolFrame& msg)
    {
      // in this context we assume the message contents are encrypted
      auto now = llarp_time_now_ms();
      if(currentIntroSet.HasExpiredIntros(now))
      {
        UpdateIntroSet();
      }
      if(selectedIntro.expiresAt <= now || now - selectedIntro.expiresAt > 1000)
      {
        ShiftIntroduction();
      }
      // XXX: this may be a different path that that was put into the protocol
      // message inside the protocol frame
      auto path = GetPathByRouter(selectedIntro.router);
      if(path)
      {
        routing::PathTransferMessage transfer(msg, selectedIntro.pathID);
        llarp::LogDebug("sending frame via ", path->Upstream(), " to ",
                        path->Endpoint(), " for ", Name());
        if(!path->SendRoutingMessage(&transfer, m_Parent->Router()))
          llarp::LogError("Failed to send frame on path");
      }
      else
      {
        llarp::LogWarn("No path to ", selectedIntro.router);
      }
    }

    std::string
    Endpoint::OutboundContext::Name() const
    {
      return "OBContext:" + m_Parent->Name() + "-"
          + currentIntroSet.A.Addr().ToString();
    }

    void
    Endpoint::OutboundContext::UpdateIntroSet()
    {
      auto addr = currentIntroSet.A.Addr();
      auto path = m_Parent->GetEstablishedPathClosestTo(addr.ToRouter());
      if(path)
      {
        HiddenServiceAddressLookup* job = new HiddenServiceAddressLookup(
            m_Parent,
            std::bind(&Endpoint::OutboundContext::OnIntroSetUpdate, this,
                      std::placeholders::_1),
            addr, m_Parent->GenTXID());

        if(!job->SendRequestViaPath(path, m_Parent->Router()))
          llarp::LogError("send via path failed");
      }
      else
      {
        llarp::LogWarn(
            "Cannot update introset no path for outbound session to ",
            currentIntroSet.A.Addr().ToString());
      }
    }

    bool
    Endpoint::OutboundContext::Tick(llarp_time_t now)
    {
      if(selectedIntro.expiresAt >= now
         || selectedIntro.expiresAt - now < 30000)
      {
        UpdateIntroSet();
      }
      m_Parent->EnsureRouterIsKnown(selectedIntro.router);
      // TODO: check for expiration of outbound context
      return false;
    }

    bool
    Endpoint::OutboundContext::SelectHop(llarp_nodedb* db, llarp_rc* prev,
                                         llarp_rc* cur, size_t hop)
    {
      if(hop == numHops - 1)
      {
        auto localcopy = llarp_nodedb_get_rc(db, selectedIntro.router);
        if(localcopy)
        {
          llarp_rc_copy(cur, localcopy);
          return true;
        }
        else
        {
          // we don't have it?
          llarp::LogError(
              "cannot build aligned path, don't have router for "
              "introduction ",
              selectedIntro);
          m_Parent->EnsureRouterIsKnown(selectedIntro.router);
          return false;
        }
      }
      else
        return llarp_pathbuilder_context::SelectHop(db, prev, cur, hop);
    }

    uint64_t
    Endpoint::GetSeqNoForConvo(const ConvoTag& tag)
    {
      auto itr = m_Sessions.find(tag);
      if(itr == m_Sessions.end())
        return 0;
      return ++(itr->second.seqno);
    }

    void
    Endpoint::OutboundContext::EncryptAndSendTo(path::Path* p,
                                                llarp_buffer_t payload,
                                                ProtocolType t)
    {
      auto path = GetPathByRouter(selectedIntro.router);
      if(path)
      {
        std::set< ConvoTag > tags;
        if(!m_Parent->m_DataHandler->GetConvoTagsForService(currentIntroSet.A,
                                                            tags))
        {
          llarp::LogError("no open converstations with remote endpoint?");
          return;
        }
        auto crypto          = m_Parent->Crypto();
        const byte_t* shared = nullptr;
        routing::PathTransferMessage msg;
        ProtocolFrame& f = msg.T;
        f.N.Randomize();
        f.T = *tags.begin();
        f.S = m_Parent->GetSeqNoForConvo(f.T);

        if(m_Parent->m_DataHandler->GetCachedSessionKeyFor(f.T, shared))
        {
          ProtocolMessage m;
          m.proto      = t;
          m.introReply = path->intro;
          m.sender     = m_Parent->m_Identity.pub;
          m.PutBuffer(payload);

          if(!f.EncryptAndSign(crypto, m, shared, m_Parent->m_Identity))
          {
            llarp::LogError("failed to sign");
            return;
          }
        }
        else
        {
          llarp::LogError("No cached session key");
          return;
        }

        msg.P = selectedIntro.pathID;
        msg.Y.Randomize();
        if(!path->SendRoutingMessage(&msg, m_Parent->Router()))
        {
          llarp::LogWarn("Failed to send routing message for data");
        }
      }
      else
      {
        llarp::LogError("no outbound path for sending message");
      }
    }

    llarp_logic*
    Endpoint::RouterLogic()
    {
      return m_Router->logic;
    }

    llarp_logic*
    Endpoint::EndpointLogic()
    {
      return m_IsolatedLogic ? m_IsolatedLogic : m_Router->logic;
    }

    llarp_crypto*
    Endpoint::Crypto()
    {
      return &m_Router->crypto;
    }

    llarp_threadpool*
    Endpoint::Worker()
    {
      return m_Router->tp;
    }

  }  // namespace service
}  // namespace llarp
