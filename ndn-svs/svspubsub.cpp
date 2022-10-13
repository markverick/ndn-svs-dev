/* -*- Mode: C++; c-file-style: "gnu"; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2012-2022 University of California, Los Angeles
 *
 * This file is part of ndn-svs, synchronization library for distributed realtime
 * applications for NDN.
 *
 * ndn-svs library is free software: you can redistribute it and/or modify it under the
 * terms of the GNU Lesser General Public License as published by the Free Software
 * Foundation, in version 2.1 of the License.
 *
 * ndn-svs library is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE. See the GNU Lesser General Public License for more details.
 */

#include "svspubsub.hpp"
#include "store-memory.hpp"
#include "tlv.hpp"

namespace ndn::svs {

SVSPubSub::SVSPubSub(const Name& syncPrefix,
                     const Name& nodePrefix,
                     ndn::Face& face,
                     const UpdateCallback& updateCallback,
                     const SecurityOptions& securityOptions,
                     std::shared_ptr<DataStore> dataStore)
  : m_syncPrefix(syncPrefix)
  , m_dataPrefix(nodePrefix)
  , m_onUpdate(updateCallback)
  , m_securityOptions(securityOptions)
  , m_svsync(syncPrefix, nodePrefix, face,
             std::bind(&SVSPubSub::updateCallbackInternal, this, _1),
             securityOptions, std::move(dataStore))
  , m_mappingProvider(syncPrefix, nodePrefix, face, securityOptions)
{
  m_svsync.getCore().setGetExtraBlockCallback(std::bind(&SVSPubSub::onGetExtraData, this, _1));
  m_svsync.getCore().setRecvExtraBlockCallback(std::bind(&SVSPubSub::onRecvExtraData, this, _1));
}

SeqNo
SVSPubSub::publish(const Name& name, const char* value, size_t length,
                   const Name& nodePrefix,
                   const time::milliseconds freshnessPeriod)
{
  auto block = ndn::encoding::makeBinaryBlock(ndn::tlv::Content, value, length);
  return publish(name, block, nodePrefix, freshnessPeriod);
}

SeqNo
SVSPubSub::publish(const Name& name, const Block& block,
                   const Name& nodePrefix,
                   const time::milliseconds freshnessPeriod)
{
  ndn::Data data(name);
  data.setContent(block);
  data.setFreshnessPeriod(freshnessPeriod);
  m_securityOptions.dataSigner->sign(data);

  return publishPacket(data, nodePrefix);
}

SeqNo
SVSPubSub::publishPacket(const Data& data, const Name& nodePrefix)
{
  NodeID nid = nodePrefix == EMPTY_NAME ? m_dataPrefix : nodePrefix;
  SeqNo seqNo = m_svsync.publishData(data.wireEncode(), data.getFreshnessPeriod(), nid, ndn::tlv::Data);

  if (m_notificationMappingList.nodeId == EMPTY_NAME || m_notificationMappingList.nodeId == nid)
  {
    m_notificationMappingList.nodeId = nid;
    m_notificationMappingList.pairs.emplace_back(seqNo, data.getName());
  }

  m_mappingProvider.insertMapping(nid, seqNo, data.getName());
  return seqNo;
}

uint32_t
SVSPubSub::subscribeToProducerPackets(const Name& nodePrefix, const PacketSubscriptionCallback& callback,
                                      bool prefetch)
{
  uint32_t handle = ++m_subscriptionCount;
  PacketSubscription sub = { handle, nodePrefix, callback, prefetch };
  m_producerSubscriptions.push_back(sub);
  return handle;
}

uint32_t
SVSPubSub::subscribeToPackets(const Name& prefix, const PacketSubscriptionCallback& callback)
{
  uint32_t handle = ++m_subscriptionCount;
  m_prefixSubscriptions.push_back(PacketSubscription{handle, prefix, callback});
  return handle;
}

void
SVSPubSub::unsubscribe(uint32_t handle)
{
  auto unsub = [](uint32_t handle, std::vector<PacketSubscription> subs)
  {
    for (size_t i = 0; i < subs.size(); i++)
    {
      if (subs[i].id == handle)
      {
        subs.erase(subs.begin() + i);
        return;
      }
    }
  };

  unsub(handle, m_producerSubscriptions);
  unsub(handle, m_prefixSubscriptions);
}

void
SVSPubSub::updateCallbackInternal(const std::vector<ndn::svs::MissingDataInfo>& info)
{
  for (const auto& stream : info)
  {
    Name streamName(stream.nodeId);

    // Producer subscriptions
    for (const auto& sub : m_producerSubscriptions)
    {
      if (sub.prefix.isPrefixOf(streamName))
      {
        // Fetch the data, validate and call callback of sub
        for (SeqNo i = stream.low; i <= stream.high; i++)
        {
          m_svsync.fetchData(stream.nodeId, i,
                             std::bind(&SVSPubSub::onSyncData, this, _1, sub, streamName, i), -1);
        }

        // Prefetch next available data
        if (sub.prefetch)
        {
          const SeqNo s = stream.high + 1;
          m_svsync.fetchData(stream.nodeId, s,
                             std::bind(&SVSPubSub::onSyncData, this, _1, sub, streamName, s), -1);
        }
      }
    }

    // Fetch all mappings if we have prefix subscription(s)
    if (!m_prefixSubscriptions.empty())
    {
      MissingDataInfo remainingInfo = stream;

      // Attemt to find what we already know about mapping
      for (SeqNo i = remainingInfo.low; i <= remainingInfo.high; i++)
      {
        try
        {
          Name mapping = m_mappingProvider.getMapping(stream.nodeId, i);
          for (const auto& sub : m_prefixSubscriptions)
          {
           if (sub.prefix.isPrefixOf(mapping))
            {
              m_svsync.fetchData(stream.nodeId, i,
                                 std::bind(&SVSPubSub::onSyncData, this, _1, sub, streamName, i), -1);
            }
          }
          remainingInfo.low++;
        }
        catch (const std::exception&)
        {
          break;
        }
      }

      // Find from network what we don't yet know
      while (remainingInfo.high >= remainingInfo.low)
      {
        // Fetch a max of 10 entries per request
        // This is to ensure the mapping response does not overflow
        // TODO: implement a better solution to this issue
        MissingDataInfo truncatedRemainingInfo = remainingInfo;
        if (truncatedRemainingInfo.high - truncatedRemainingInfo.low > 10)
        {
          truncatedRemainingInfo.high = truncatedRemainingInfo.low + 10;
        }

        m_mappingProvider.fetchNameMapping(truncatedRemainingInfo,
          [this, remainingInfo, streamName] (const MappingList& list) {
            for (const auto& sub : m_prefixSubscriptions)
            {
              for (const auto& [seq, name] : list.pairs)
              {
                if (sub.prefix.isPrefixOf(name))
                {
                  m_svsync.fetchData(remainingInfo.nodeId, seq,
                                     std::bind(&SVSPubSub::onSyncData, this, _1, sub, streamName, seq), -1);
                }
              }
            }
          }, -1);

        remainingInfo.low += 11;
      }
    }
  }

  m_onUpdate(info);
}

bool
SVSPubSub::onSyncData(const Data& syncData, const PacketSubscription& subscription,
                      const Name& streamName, SeqNo seqNo)
{
  // Check for duplicate calls and push into queue
  {
    const size_t hash = std::hash<Name>{}(Name(streamName).appendNumber(seqNo));
    const auto& ht = m_receivedObjectIds.get<Hashtable>();
    if (ht.find(hash) != ht.end())
      return false;
    m_receivedObjectIds.get<Sequence>().push_back(hash);

    // Pop out the oldest entry if the queue is full
    if (m_receivedObjectIds.get<Sequence>().size() > MAX_OBJECT_IDS)
      m_receivedObjectIds.get<Sequence>().pop_front();
  }

  // Check if data in encapsulated
  if (syncData.getContentType() == ndn::tlv::Data)
  {
    Data encapsulatedData(syncData.getContent().blockFromValue());

    try {
      m_mappingProvider.getMapping(streamName, seqNo);
    }
    catch (const std::exception&) {
      m_mappingProvider.insertMapping(streamName, seqNo, encapsulatedData.getName());
    }

    // Return data
    SubscriptionPacket subPack = { encapsulatedData, streamName, seqNo };

    if (static_cast<bool>(m_securityOptions.encapsulatedDataValidator)) {
      m_securityOptions.encapsulatedDataValidator->validate(
        encapsulatedData,
        [&] (const Data&) { subscription.callback(subPack); },
        [] (auto&&...) {});
    }
    else {
      subscription.callback(subPack);
    }
    return true;
  }

  return false;
}

Block
SVSPubSub::onGetExtraData(const VersionVector&)
{
  MappingList copy = m_notificationMappingList;
  m_notificationMappingList = MappingList();
  return copy.encode();
}

void
SVSPubSub::onRecvExtraData(const Block& block)
{
  try
  {
    MappingList list(block);
    for (const auto& p : list.pairs)
    {
      m_mappingProvider.insertMapping(list.nodeId, p.first, p.second);
    }
  }
  catch (const std::exception&) {}
}

} // namespace ndn::svs
