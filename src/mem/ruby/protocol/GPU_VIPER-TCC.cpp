/*
 * Copyright (c) 2010-2015 Advanced Micro Devices, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from this
 * software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * Author: Blake Hechtman
 */

machine(MachineType:TCC, "TCC Cache")
 : CacheMemory * L2cache;
   bool WB; /*is this cache Writeback?*/
   Cycles l2_request_latency := 50;
   Cycles l2_response_latency := 20;
   Cycles glc_atomic_latency := 0;

  // From the TCPs or SQCs
  MessageBuffer * requestFromTCP, network="From", virtual_network="1", vnet_type="request";
  // To the Cores. TCC deals only with TCPs/SQCs.
  MessageBuffer * responseToCore, network="To", virtual_network="3", vnet_type="response";
  // From the NB
  MessageBuffer * probeFromNB, network="From", virtual_network="0", vnet_type="request";
  MessageBuffer * responseFromNB, network="From", virtual_network="2", vnet_type="response";
  // To the NB
  MessageBuffer * requestToNB, network="To", virtual_network="0", vnet_type="request";
  MessageBuffer * responseToNB, network="To", virtual_network="2", vnet_type="response";
  MessageBuffer * unblockToNB, network="To", virtual_network="4", vnet_type="unblock";

  MessageBuffer * triggerQueue;

{
  // EVENTS
  enumeration(Event, desc="TCC Events") {
    // Requests coming from the Cores
    RdBlk,                  desc="RdBlk event";
    RdBypassEvict,          desc="Bypass L2 on reads. Evict if cache block already allocated";
    WrVicBlk,               desc="L1 Write Through";
    WrVicBlkBack,           desc="L1 Write Through(dirty cache)";
    WrVicBlkEvict,          desc="L1 Write Through(dirty cache) and evict";
    AtomicWait,             desc="Atomic Op that must wait for pending loads";
    Atomic,                 desc="Atomic Op";
    AtomicPassOn,           desc="Atomic Op Passed on to Directory";
    AtomicDone,             desc="AtomicOps Complete";
    AtomicNotDone,          desc="AtomicOps not Complete";
    Data,                   desc="Data message";
    Flush,                  desc="Flush cache entry";
    // Coming from this TCC
    L2_Repl,                desc="L2 Replacement";
    // Probes
    PrbInv,                 desc="Invalidating probe";
    // Coming from Memory Controller
    WBAck,                  desc="writethrough ack from memory";
    Bypass,                 desc="Bypass the entire L2 cache";
  }

  // STATES
  state_declaration(State, desc="TCC State", default="TCC_State_I") {
    M, AccessPermission:Read_Write, desc="Modified(dirty cache only)";
    W, AccessPermission:Read_Write, desc="Written(dirty cache only)";
    V, AccessPermission:Read_Only,  desc="Valid";
    I, AccessPermission:Invalid,    desc="Invalid";
    IV, AccessPermission:Busy,      desc="Waiting for Data";
    WI, AccessPermission:Busy,      desc="Waiting on Writethrough Ack";
    WIB, AccessPermission:Busy,     desc="Waiting on Writethrough Ack; Will be Bypassed";
    A, AccessPermission:Busy,       desc="Invalid waiting on atomici Data";
  }

  enumeration(RequestType, desc="To communicate stats from transitions to recordStats") {
    DataArrayRead,    desc="Read the data array";
    DataArrayWrite,   desc="Write the data array";
    TagArrayRead,     desc="Read the data array";
    TagArrayWrite,    desc="Write the data array";
    AtomicALUOperation,  desc="Atomic ALU operation";
  }


  // STRUCTURES

  structure(Entry, desc="...", interface="AbstractCacheEntry") {
    State CacheState,           desc="cache state";
    bool Dirty,                 desc="Is the data dirty (diff from memory?)";
    DataBlock DataBlk,          desc="Data for the block";
    WriteMask writeMask,        desc="Dirty byte mask";
  }

  structure(TBE, desc="...") {
    State TBEState,                  desc="Transient state";
    DataBlock DataBlk,               desc="data for the block";
    bool Dirty,                      desc="Is the data dirty?";
    bool Shared,                     desc="Victim hit by shared probe";
    MachineID From,                  desc="Waiting for writeback from...";
    NetDest Destination,             desc="Data destination";
    int numPending,                  desc="num pending requests";
    int numPendingDirectoryAtomics,  desc="number of pending atomics to be performed in directory";
    int atomicDoneCnt,               desc="number AtomicDones triggered";
    bool isGLCSet,                   desc="Bypass L1 Cache";
    bool isSLCSet,                   desc="Bypass L1 and L2 Cache";
    WriteMask atomicWriteMask,       desc="Atomic write mask";
  }

  structure(TBETable, external="yes") {
    TBE lookup(Addr);
    void allocate(Addr);
    void deallocate(Addr);
    bool isPresent(Addr);
  }

  TBETable TBEs, template="<TCC_TBE>", constructor="m_number_of_TBEs";

  void set_cache_entry(AbstractCacheEntry b);
  void unset_cache_entry();
  void set_tbe(TBE b);
  void unset_tbe();
  void wakeUpAllBuffers();
  void wakeUpBuffers(Addr a);
  void wakeUpAllBuffers(Addr a);

  MachineID mapAddressToMachine(Addr addr, MachineType mtype);

  // FUNCTION DEFINITIONS
  Tick clockEdge();

  Entry getCacheEntry(Addr addr), return_by_pointer="yes" {
    return static_cast(Entry, "pointer", L2cache.lookup(addr));
  }

  DataBlock getDataBlock(Addr addr), return_by_ref="yes" {
    return getCacheEntry(addr).DataBlk;
  }

  bool presentOrAvail(Addr addr) {
    return L2cache.isTagPresent(addr) || L2cache.cacheAvail(addr);
  }

  State getState(TBE tbe, Entry cache_entry, Addr addr) {
    if (is_valid(tbe)) {
      return tbe.TBEState;
    } else if (is_valid(cache_entry)) {
      return cache_entry.CacheState;
    }
    return State:I;
  }

  void setState(TBE tbe, Entry cache_entry, Addr addr, State state) {
    if (is_valid(tbe)) {
        tbe.TBEState := state;
    }

    if (is_valid(cache_entry)) {
        cache_entry.CacheState := state;
    }
  }

  void functionalRead(Addr addr, Packet *pkt) {
    TBE tbe := TBEs.lookup(addr);
    if(is_valid(tbe)) {
      testAndRead(addr, tbe.DataBlk, pkt);
    } else {
      functionalMemoryRead(pkt);
    }
  }

  int functionalWrite(Addr addr, Packet *pkt) {
    int num_functional_writes := 0;
    TBE tbe := TBEs.lookup(addr);
    if(is_valid(tbe)) {
      num_functional_writes := num_functional_writes +
            testAndWrite(addr, tbe.DataBlk, pkt);
    }

    num_functional_writes := num_functional_writes +
        functionalMemoryWrite(pkt);
    return num_functional_writes;
  }

  AccessPermission getAccessPermission(Addr addr) {
    TBE tbe := TBEs.lookup(addr);
    if(is_valid(tbe)) {
      return TCC_State_to_permission(tbe.TBEState);
    }

    Entry cache_entry := getCacheEntry(addr);
    if(is_valid(cache_entry)) {
      return TCC_State_to_permission(cache_entry.CacheState);
    }

    return AccessPermission:NotPresent;
  }

  void setAccessPermission(Entry cache_entry, Addr addr, State state) {
    if (is_valid(cache_entry)) {
      cache_entry.changePermission(TCC_State_to_permission(state));
    }
  }

  void recordRequestType(RequestType request_type, Addr addr) {
    if (request_type == RequestType:DataArrayRead) {
        L2cache.recordRequestType(CacheRequestType:DataArrayRead, addr);
    } else if (request_type == RequestType:DataArrayWrite) {
        L2cache.recordRequestType(CacheRequestType:DataArrayWrite, addr);
    } else if (request_type == RequestType:TagArrayRead) {
        L2cache.recordRequestType(CacheRequestType:TagArrayRead, addr);
    } else if (request_type == RequestType:TagArrayWrite) {
        L2cache.recordRequestType(CacheRequestType:TagArrayWrite, addr);
    } else if (request_type == RequestType:AtomicALUOperation) {
        L2cache.recordRequestType(CacheRequestType:AtomicALUOperation, addr);
    }
  }

  bool checkResourceAvailable(RequestType request_type, Addr addr) {
    if (request_type == RequestType:DataArrayRead) {
      return L2cache.checkResourceAvailable(CacheResourceType:DataArray, addr);
    } else if (request_type == RequestType:DataArrayWrite) {
      return L2cache.checkResourceAvailable(CacheResourceType:DataArray, addr);
    } else if (request_type == RequestType:TagArrayRead) {
      return L2cache.checkResourceAvailable(CacheResourceType:TagArray, addr);
    } else if (request_type == RequestType:TagArrayWrite) {
      return L2cache.checkResourceAvailable(CacheResourceType:TagArray, addr);
    } else if (request_type == RequestType:AtomicALUOperation) {
      return L2cache.checkResourceAvailable(CacheResourceType:AtomicALUArray, addr);
    } else {
      error("Invalid RequestType type in checkResourceAvailable");
      return true;
    }
  }


  // ** OUT_PORTS **

  // Three classes of ports
  // Class 1: downward facing network links to NB
  out_port(requestToNB_out, CPURequestMsg, requestToNB);
  out_port(responseToNB_out, ResponseMsg, responseToNB);
  out_port(unblockToNB_out, UnblockMsg, unblockToNB);

  // Class 2: upward facing ports to GPU cores
  out_port(responseToCore_out, ResponseMsg, responseToCore);

  out_port(triggerQueue_out, TriggerMsg, triggerQueue);
  //
  // request queue going to NB
  //

  // ** IN_PORTS **
  in_port(triggerQueue_in, TriggerMsg, triggerQueue) {
    if (triggerQueue_in.isReady(clockEdge())) {
      peek(triggerQueue_in, TriggerMsg) {
        TBE tbe := TBEs.lookup(in_msg.addr);
        Entry cache_entry := getCacheEntry(in_msg.addr);

        // The trigger queue applies only to atomics performed in the directory.

        // There is a possible race where multiple AtomicDone triggers can be
        // sent if another Atomic to the same address is issued after the
        // AtomicDone is triggered but before the message arrives here. For
        // that case we count the number of AtomicDones in flight for this
        // address and only call AtomicDone to deallocate the TBE when it is
        // the last in flight message.
        if (tbe.numPendingDirectoryAtomics == 0 && tbe.atomicDoneCnt == 1) {
            trigger(Event:AtomicDone, in_msg.addr, cache_entry, tbe);
        } else {
            trigger(Event:AtomicNotDone, in_msg.addr, cache_entry, tbe);
        }
      }
    }
  }

  // handle responses from directory here
  in_port(responseFromNB_in, ResponseMsg, responseFromNB) {
    if (responseFromNB_in.isReady(clockEdge())) {
      peek(responseFromNB_in, ResponseMsg, block_on="addr") {
        TBE tbe := TBEs.lookup(in_msg.addr);
        Entry cache_entry := getCacheEntry(in_msg.addr);
        /*
          MOESI_AMD_Base-dir acts as the directory, and it always passes
          SLC information back to L2 because of races at L2 with requests
          from different CUs sending requests to same cache line in parallel.
          If these requests have different GLC/SLC settings, the L2 TBE may
          not have the correct GLC/SLC information for a given request.
         */
        bool is_slc_set := in_msg.isSLCSet;

        // Whether the SLC bit is set or not, WB acks should invoke the
        // WBAck event. For cases where a read response will follow a
        // WBAck (A read bypass evict on a dirty line), the line's TLB
        // will not be deallocated on WBAck, and the SLC bit will be
        // checked when the read response is received.
        if (in_msg.Type == CoherenceResponseType:NBSysWBAck) {
          trigger(Event:WBAck, in_msg.addr, cache_entry, tbe);
        } else if(in_msg.Type == CoherenceResponseType:NBSysResp) {
          // If the SLC bit is set or the cache is write-through and
          // we're receiving modified data (such as from an atomic),
          // the response needs to bypass the cache and should not be
          // allocated an entry.
          if(is_slc_set || (!WB && in_msg.State == CoherenceState:Modified)) {
            trigger(Event:Bypass, in_msg.addr, cache_entry, tbe);
          } else {
            if(presentOrAvail(in_msg.addr)) {
              // Responses with atomic data will only reach here if the
              // SLC bit isn't set and the cache is WB
              trigger(Event:Data, in_msg.addr, cache_entry, tbe);
            } else {
              Addr victim :=  L2cache.cacheProbe(in_msg.addr);
              trigger(Event:L2_Repl, victim, getCacheEntry(victim), TBEs.lookup(victim));
            }
          }
        } else {
          error("Unexpected Response Message to Core");
        }
      }
    }
  }

  // Finally handling incoming requests (from TCP) and probes (from NB).
  in_port(probeNetwork_in, NBProbeRequestMsg, probeFromNB) {
    if (probeNetwork_in.isReady(clockEdge())) {
      peek(probeNetwork_in, NBProbeRequestMsg) {
        DPRINTF(RubySlicc, "%s\n", in_msg);
        Entry cache_entry := getCacheEntry(in_msg.addr);
        TBE tbe := TBEs.lookup(in_msg.addr);
        trigger(Event:PrbInv, in_msg.addr, cache_entry, tbe);
      }
    }
  }

  in_port(coreRequestNetwork_in, CPURequestMsg, requestFromTCP, rank=0) {
    if (coreRequestNetwork_in.isReady(clockEdge())) {
      peek(coreRequestNetwork_in, CPURequestMsg) {
        TBE tbe := TBEs.lookup(in_msg.addr);
        Entry cache_entry := getCacheEntry(in_msg.addr);
        if (in_msg.Type == CoherenceRequestType:WriteThrough) {
            if (in_msg.isSLCSet) {
                // The request should bypass the cache if SLC bit is set.
                // If the cache entry exists already, then evict it.
                // Else, perform a normal cache access.
                // The cache entry is allocated only on response and bypass is
                // handled there
                if(presentOrAvail(in_msg.addr)) {
                    trigger(Event:WrVicBlkEvict, in_msg.addr, cache_entry, tbe);
                } else {
                    trigger(Event:WrVicBlk, in_msg.addr, cache_entry, tbe);
                }
            } else if(WB) {
                if(presentOrAvail(in_msg.addr)) {
                    trigger(Event:WrVicBlkBack, in_msg.addr, cache_entry, tbe);
                } else {
                    Addr victim :=  L2cache.cacheProbe(in_msg.addr);
                    trigger(Event:L2_Repl, victim, getCacheEntry(victim), TBEs.lookup(victim));
                }
            } else {
                trigger(Event:WrVicBlk, in_msg.addr, cache_entry, tbe);
            }
        } else if (in_msg.Type == CoherenceRequestType:Atomic ||
                   in_msg.Type == CoherenceRequestType:AtomicReturn ||
                   in_msg.Type == CoherenceRequestType:AtomicNoReturn) {
	  /*
	    If there are pending requests for this line already and those
	    requests are not atomics, because we can't easily differentiate
	    between different request types on return and because decrementing
	    the atomic count assumes all returned requests in the A state are
	    atomics, we will need to put this atomic to sleep and wake it up
	    when the loads return.
	   */
	  if (is_valid(tbe) && (tbe.numPending > 0) &&
	        (tbe.numPendingDirectoryAtomics == 0)) {
            trigger(Event:AtomicWait, in_msg.addr, cache_entry, tbe);
          } else {
            // If the request is system-level, if the address isn't in the cache,
            // or if this cache is write-through, then send the request to the
            // directory. Since non-SLC atomics won't be performed by the directory,
            // TCC will perform the atomic on the return path on Event:Data.
            // The action will invalidate the cache line if SLC is set and the address is
            // in the cache.
            if(in_msg.isSLCSet || !WB) {
              trigger(Event:AtomicPassOn, in_msg.addr, cache_entry, tbe);
            } else {
              trigger(Event:Atomic, in_msg.addr, cache_entry, tbe);
            }
          }
        } else if (in_msg.Type == CoherenceRequestType:RdBlk) {
          if (in_msg.isSLCSet) {
            // If SLC bit is set, the request needs to go directly to memory.
            // If a cache block already exists, then evict it.
            trigger(Event:RdBypassEvict, in_msg.addr, cache_entry, tbe);
          } else {
            trigger(Event:RdBlk, in_msg.addr, cache_entry, tbe);
          }
        } else if (in_msg.Type == CoherenceRequestType:WriteFlush) {
            trigger(Event:Flush, in_msg.addr, cache_entry, tbe);
        } else {
          DPRINTF(RubySlicc, "%s\n", in_msg);
          error("Unexpected Response Message to Core");
        }
      }
    }
  }
  // BEGIN ACTIONS

  action(i_invL2, "i", desc="invalidate TCC cache block") {
    if (is_valid(cache_entry)) {
        L2cache.deallocate(address);
    }
    unset_cache_entry();
  }

  action(sd_sendData, "sd", desc="send Shared response") {
    peek(coreRequestNetwork_in, CPURequestMsg) {
      enqueue(responseToCore_out, ResponseMsg, l2_response_latency) {
        out_msg.addr := address;
        out_msg.Type := CoherenceResponseType:TDSysResp;
        out_msg.Sender := machineID;
        out_msg.Destination.add(in_msg.Requestor);
        out_msg.DataBlk := cache_entry.DataBlk;
        out_msg.MessageSize := MessageSizeType:Response_Data;
        out_msg.Dirty := false;
        out_msg.State := CoherenceState:Shared;
        out_msg.isGLCSet := in_msg.isGLCSet;
        out_msg.isSLCSet := in_msg.isSLCSet;
        DPRINTF(RubySlicc, "%s\n", out_msg);
      }
    }
  }


  action(sdr_sendDataResponse, "sdr", desc="send Shared response") {
    enqueue(responseToCore_out, ResponseMsg, l2_response_latency) {
      out_msg.addr := address;
      out_msg.Type := CoherenceResponseType:TDSysResp;
      out_msg.Sender := machineID;
      out_msg.MessageSize := MessageSizeType:Response_Data;
      out_msg.Dirty := false;
      out_msg.State := CoherenceState:Shared;
      peek(responseFromNB_in, ResponseMsg) {
        // if line state is Invalid, then we must be doing the transition(I, Data)
        // so use the DataBlk from the incoming message
        if ((getAccessPermission(address) == AccessPermission:NotPresent) ||
	      (getAccessPermission(address) == AccessPermission:Invalid)) {
          out_msg.DataBlk := in_msg.DataBlk;
        } else {
          out_msg.DataBlk := cache_entry.DataBlk;
        }
        out_msg.isGLCSet := in_msg.isGLCSet;
        out_msg.isSLCSet := in_msg.isSLCSet;
        // reuse CURequestor field to allow multiple concurrent loads and
        // track where they should go back to (since TBE can't distinguish
        // destinations)
        out_msg.Destination.clear();
        out_msg.Destination.add(in_msg.CURequestor);
      }
      DPRINTF(RubySlicc, "%s\n", out_msg);
    }
    enqueue(unblockToNB_out, UnblockMsg, 1) {
      out_msg.addr := address;
      out_msg.Destination.add(mapAddressToMachine(address, MachineType:Directory));
      out_msg.MessageSize := MessageSizeType:Unblock_Control;
      peek(responseFromNB_in, ResponseMsg) {
        out_msg.isGLCSet := in_msg.isGLCSet;
        out_msg.isSLCSet := in_msg.isSLCSet;
      }
      DPRINTF(RubySlicc, "%s\n", out_msg);
    }
  }

  action(rb_bypassDone, "rb", desc="bypass L2 of read access") {
    peek(responseFromNB_in, ResponseMsg) {
        enqueue(responseToCore_out, ResponseMsg, l2_response_latency) {
          out_msg.addr := address;
          out_msg.Type := CoherenceResponseType:TDSysResp;
          out_msg.Sender := machineID;
          // reuse CURequestor field to allow multiple concurrent loads and
          // track where they should go back to (since TBE can't distinguish
          // destinations)
          out_msg.Destination.clear();
          out_msg.Destination.add(in_msg.CURequestor);
          out_msg.DataBlk := in_msg.DataBlk;
          out_msg.MessageSize := MessageSizeType:Response_Data;
          out_msg.Dirty := false;
          out_msg.State := CoherenceState:Shared;
          out_msg.isGLCSet := in_msg.isGLCSet;
          out_msg.isSLCSet := in_msg.isSLCSet;
          DPRINTF(RubySlicc, "%s\n", out_msg);
        }
        enqueue(unblockToNB_out, UnblockMsg, 1) {
          out_msg.addr := address;
          out_msg.Destination.add(mapAddressToMachine(address, MachineType:Directory));
          out_msg.MessageSize := MessageSizeType:Unblock_Control;
          DPRINTF(RubySlicc, "%s\n", out_msg);
        }
    }
  }

  action(rd_requestData, "r", desc="Miss in L2, pass on") {
    peek(coreRequestNetwork_in, CPURequestMsg) {
      DPRINTF(RubySlicc, "in_msg: %s\n", in_msg);
      enqueue(requestToNB_out, CPURequestMsg, l2_request_latency) {
        out_msg.addr := address;
        out_msg.Type := in_msg.Type;
        out_msg.Requestor := machineID;
        /*
          To allow multiple concurrent requests from different CUs, we pass
          the orgin information along to the directory, which stores it in its
          TBE as appropriate before passing it back to the TCC on the return
          path.
         */
        out_msg.CURequestor := in_msg.Requestor;
        out_msg.Destination.add(mapAddressToMachine(address, MachineType:Directory));
        out_msg.Shared := false; // unneeded for this request
        out_msg.MessageSize := in_msg.MessageSize;
        out_msg.isGLCSet := in_msg.isGLCSet;
        out_msg.isSLCSet := in_msg.isSLCSet;
        DPRINTF(RubySlicc, "out_msg: %s\n", out_msg);
      }
    }
  }

  action(w_sendResponseWBAck, "w", desc="send WB Ack") {
    peek(responseFromNB_in, ResponseMsg) {
      enqueue(responseToCore_out, ResponseMsg, l2_response_latency) {
        out_msg.addr := address;
        out_msg.Type := CoherenceResponseType:TDSysWBAck;
        out_msg.Destination.clear();
        out_msg.Destination.add(in_msg.CURequestor);
        out_msg.Sender := machineID;
        out_msg.MessageSize := MessageSizeType:Writeback_Control;
        out_msg.instSeqNum := in_msg.instSeqNum;
      }
    }
  }

  action(swb_sendWBAck, "swb", desc="send WB Ack") {
    peek(coreRequestNetwork_in, CPURequestMsg) {
      enqueue(responseToCore_out, ResponseMsg, l2_response_latency) {
        out_msg.addr := address;
        out_msg.Type := CoherenceResponseType:TDSysWBAck;
        out_msg.Destination.clear();
        out_msg.Destination.add(in_msg.Requestor);
        out_msg.Sender := machineID;
        out_msg.MessageSize := MessageSizeType:Writeback_Control;
        out_msg.instSeqNum := in_msg.instSeqNum;
      }
    }
  }

  action(fw_sendFlushResponse, "fw", desc="send Flush Response") {
    peek(coreRequestNetwork_in, CPURequestMsg) {
      enqueue(responseToCore_out, ResponseMsg, l2_response_latency) {
        out_msg.addr := address;
        out_msg.Type := CoherenceResponseType:TDSysWBAck;
        out_msg.Destination.clear();
        out_msg.Destination.add(in_msg.Requestor);
        out_msg.Sender := machineID;
        out_msg.MessageSize := MessageSizeType:Writeback_Control;
        out_msg.instSeqNum := in_msg.instSeqNum;
      }
    }
  }

  action(ar_sendAtomicResponse, "ar", desc="send Atomic Ack") {
    peek(coreRequestNetwork_in, CPURequestMsg) {
        enqueue(responseToCore_out, ResponseMsg, l2_response_latency + glc_atomic_latency, true) {
          out_msg.addr := address;
          out_msg.Type := CoherenceResponseType:TDSysResp;
          out_msg.Destination.clear();
          out_msg.Destination.add(in_msg.Requestor);
          out_msg.Sender := machineID;
          out_msg.MessageSize := MessageSizeType:Response_Data;
          out_msg.DataBlk := cache_entry.DataBlk;
          out_msg.isGLCSet := in_msg.isGLCSet;
          out_msg.isSLCSet := in_msg.isSLCSet;
        }
    }
    cache_entry.DataBlk.clearAtomicLogEntries();
  }

  action(baplr_sendBypassedAtomicPerformedLocallyResponse, "barplr", desc="send locally-performed bypassed Atomic Ack") {
    peek(responseFromNB_in, ResponseMsg) {
        enqueue(responseToCore_out, ResponseMsg, l2_response_latency) {
          out_msg.addr := address;
          out_msg.Type := CoherenceResponseType:TDSysResp;
          out_msg.Destination.add(in_msg.CURequestor);
          out_msg.Sender := machineID;
          out_msg.MessageSize := in_msg.MessageSize;
          out_msg.DataBlk := cache_entry.DataBlk;
          out_msg.isGLCSet := tbe.isGLCSet;
          out_msg.isSLCSet := tbe.isSLCSet;
        }
    }
    cache_entry.DataBlk.clearAtomicLogEntries();
  }

  action(bapdr_sendBypassedAtomicPerformedInDirectoryResponse, "bapdr", desc="send bypassed Atomic Ack") {
    peek(responseFromNB_in, ResponseMsg) {
        enqueue(responseToCore_out, ResponseMsg, l2_response_latency) {
          out_msg.addr := address;
          out_msg.Type := CoherenceResponseType:TDSysResp;
          out_msg.Destination.add(in_msg.CURequestor);
          out_msg.Sender := machineID;
          out_msg.MessageSize := in_msg.MessageSize;
          out_msg.DataBlk := in_msg.DataBlk;
          out_msg.isGLCSet := in_msg.isGLCSet;
          out_msg.isSLCSet := in_msg.isSLCSet;
        }
    }
  }

  action(a_allocateBlock, "a", desc="allocate TCC block") {
    if (is_invalid(cache_entry)) {
      set_cache_entry(L2cache.allocate(address, new Entry));
      cache_entry.writeMask.clear();
    }
  }

  action(p_profileMiss, "pm", desc="Profile cache miss") {
      L2cache.profileDemandMiss();
  }

  action(p_profileHit, "ph", desc="Profile cache hit") {
      L2cache.profileDemandHit();
  }

  action(t_allocateTBE, "t", desc="allocate TBE Entry") {
    if (is_invalid(tbe)) {
      check_allocate(TBEs);
      TBEs.allocate(address);
      set_tbe(TBEs.lookup(address));
      tbe.Destination.clear();
      tbe.numPendingDirectoryAtomics := 0;
      tbe.atomicDoneCnt := 0;
      tbe.numPending := 0;
    }
    // each pending requests increments this count by 1
    tbe.numPending := tbe.numPending + 1;
    if (coreRequestNetwork_in.isReady(clockEdge())) {
      peek(coreRequestNetwork_in, CPURequestMsg) {
        if(in_msg.Type == CoherenceRequestType:RdBlk ||
           in_msg.Type == CoherenceRequestType:Atomic ||
           in_msg.Type == CoherenceRequestType:AtomicReturn ||
           in_msg.Type == CoherenceRequestType:AtomicNoReturn){
          tbe.Destination.add(in_msg.Requestor);
        }
        /*
          If there are multiple concurrent requests to the same cache line, each
          one will overwrite the previous ones GLC/SLC information here.
          If these requests have different GLC/SLC information, this causes
          a segfault.  Hence, currently the support relies on the directory to
          pass back the GLC/SLC information instead of relying on L2 TBE to be
          correct.

          This message is left here as an FYI for future developers.
         */
        tbe.isGLCSet := in_msg.isGLCSet;
        tbe.isSLCSet := in_msg.isSLCSet;
        if(in_msg.Type == CoherenceRequestType:Atomic ||
           in_msg.Type == CoherenceRequestType:AtomicReturn ||
           in_msg.Type == CoherenceRequestType:AtomicNoReturn){
          tbe.atomicWriteMask.clear();
          tbe.atomicWriteMask.orMask(in_msg.writeMask);
        }
      }
    }
  }

  action(dt_deallocateTBE, "dt", desc="Deallocate TBE entry") {
    // since we may have multiple destinations, can't deallocate if we aren't
    // last one
    tbe.numPending := tbe.numPending - 1;
    if (tbe.numPending == 0) {
      tbe.Destination.clear();
      TBEs.deallocate(address);
      unset_tbe();
    }
  }

  action(wcb_writeCacheBlock, "wcb", desc="write data to TCC") {
    peek(responseFromNB_in, ResponseMsg) {
      cache_entry.DataBlk := in_msg.DataBlk;
      DPRINTF(RubySlicc, "Writing to TCC: %s\n", in_msg);
    }
  }

  action(wdb_writeDirtyBytes, "wdb", desc="write data to TCC") {
    peek(coreRequestNetwork_in, CPURequestMsg) {
      cache_entry.DataBlk.copyPartial(in_msg.DataBlk,in_msg.writeMask);
      cache_entry.writeMask.orMask(in_msg.writeMask);
      DPRINTF(RubySlicc, "Writing to TCC: %s\n", in_msg);
    }
  }

  action(wardb_writeAtomicResponseDirtyBytes, "wardb", desc="write data to TCC") {
    peek(responseFromNB_in, ResponseMsg) {
      cache_entry.DataBlk := in_msg.DataBlk;
      cache_entry.writeMask.orMask(tbe.atomicWriteMask);
      DPRINTF(RubySlicc, "Writing to TCC: %s\n", in_msg);
    }
  }

  action(owm_orWriteMask, "owm", desc="or TCCs write mask") {
    peek(coreRequestNetwork_in, CPURequestMsg) {
      cache_entry.writeMask.orMask(in_msg.writeMask);
    }
  }

  action(wt_writeThrough, "wt", desc="write back data") {
    peek(coreRequestNetwork_in, CPURequestMsg) {
      enqueue(requestToNB_out, CPURequestMsg, l2_request_latency) {
        out_msg.addr := address;
        out_msg.Requestor := machineID;
        out_msg.CURequestor := in_msg.Requestor;
        out_msg.Destination.add(mapAddressToMachine(address, MachineType:Directory));
        out_msg.MessageSize := MessageSizeType:Data;
        out_msg.Type := CoherenceRequestType:WriteThrough;
        out_msg.Dirty := true;
        out_msg.DataBlk := in_msg.DataBlk;
        out_msg.writeMask.orMask(in_msg.writeMask);
        out_msg.instSeqNum := in_msg.instSeqNum;
        out_msg.isGLCSet := in_msg.isGLCSet;
        out_msg.isSLCSet := in_msg.isSLCSet;
      }
    }
  }

  action(wb_writeBack, "wb", desc="write back data") {
    enqueue(requestToNB_out, CPURequestMsg, l2_request_latency) {
      out_msg.addr := address;
      out_msg.Requestor := machineID;
      out_msg.CURequestor := machineID;
      out_msg.Destination.add(mapAddressToMachine(address, MachineType:Directory));
      out_msg.MessageSize := MessageSizeType:Data;
      out_msg.Type := CoherenceRequestType:WriteThrough;
      out_msg.Dirty := true;
      out_msg.DataBlk := cache_entry.DataBlk;
      out_msg.writeMask.orMask(cache_entry.writeMask);
    }
  }

  action(f_flush, "f", desc="write back data") {
    peek(coreRequestNetwork_in, CPURequestMsg) {
      enqueue(requestToNB_out, CPURequestMsg, l2_request_latency) {
        out_msg.addr := address;
        out_msg.Requestor := machineID;
        out_msg.CURequestor := in_msg.Requestor;
        out_msg.Destination.add(mapAddressToMachine(address, MachineType:Directory));
        out_msg.MessageSize := MessageSizeType:Data;
        out_msg.Type := CoherenceRequestType:WriteFlush;
        out_msg.Dirty := true;
        out_msg.DataBlk := cache_entry.DataBlk;
        out_msg.writeMask.orMask(cache_entry.writeMask);
        out_msg.isGLCSet := in_msg.isGLCSet;
        out_msg.isSLCSet := in_msg.isSLCSet;
      }
    }
  }

  action(at_atomicThrough, "at", desc="write back data") {
    peek(coreRequestNetwork_in, CPURequestMsg) {
      enqueue(requestToNB_out, CPURequestMsg, l2_request_latency) {
        out_msg.addr := address;
        out_msg.Requestor := machineID;
        out_msg.CURequestor := in_msg.Requestor;
        out_msg.Destination.add(mapAddressToMachine(address, MachineType:Directory));
        out_msg.MessageSize := MessageSizeType:Data;
        out_msg.Type := in_msg.Type;
        out_msg.Dirty := true;
        out_msg.writeMask.orMask(in_msg.writeMask);
        out_msg.isGLCSet := in_msg.isGLCSet;
        out_msg.isSLCSet := in_msg.isSLCSet;
      }
    }
  }

  action(pi_sendProbeResponseInv, "pi", desc="send probe ack inv, no data") {
    enqueue(responseToNB_out, ResponseMsg, 1) {
      out_msg.addr := address;
      out_msg.Type := CoherenceResponseType:CPUPrbResp;  // TCC, L3  respond in same way to probes
      out_msg.Sender := machineID;
      out_msg.Destination.add(mapAddressToMachine(address, MachineType:Directory));
      out_msg.Dirty := false;
      out_msg.Hit := false;
      out_msg.Ntsl := true;
      out_msg.State := CoherenceState:NA;
      out_msg.MessageSize := MessageSizeType:Response_Control;
    }
  }
  action(ut_updateTag, "ut", desc="update Tag (i.e. set MRU)") {
    L2cache.setMRU(address);
  }

  action(p_popRequestQueue, "p", desc="pop request queue") {
    coreRequestNetwork_in.dequeue(clockEdge());
  }

  action(pr_popResponseQueue, "pr", desc="pop response queue") {
    responseFromNB_in.dequeue(clockEdge());
  }

  action(pp_popProbeQueue, "pp", desc="pop probe queue") {
    probeNetwork_in.dequeue(clockEdge());
  }

  action(st_stallAndWaitRequest, "st", desc="Stall and wait on the address") {
    stall_and_wait(coreRequestNetwork_in, address);
  }

  action(wada_wakeUpAllDependentsAddr, "wada", desc="Wake up any requests waiting for this address") {
    wakeUpAllBuffers(address);
  }

  /*
    Currently z_stall is unused because it can lead to Protocol Stalls that
    eventually lead to deadlock.  Instead, it is recommended to use
    st_stallAndWaitRequest in combination with a wakeupBuffer call (e.g.,
    wada_wakeUpAllDependentsAddr) to put the pending requests to sleep instead of
    them causing head of line blocking -- wada_wakeUpAllDependentsAddr should wake
    the request up once the request preventing it from completing is done.
  action(z_stall, "z", desc="stall") {
      // built-in
  }
  */


  action(inpa_incrementNumPendingDirectoryAtomics, "inpa", desc="inc num atomics") {
    // Only increment number of atomics if they will actually be performed in directory
    // That is, if the SLC bit is set or if the cache is write through
    peek(coreRequestNetwork_in, CPURequestMsg) {
      if (in_msg.isSLCSet || !WB) {
        tbe.numPendingDirectoryAtomics := tbe.numPendingDirectoryAtomics + 1;
      }
    }
  }


  action(dnpa_decrementNumPendingDirectoryAtomics, "dnpa", desc="dec num atomics") {
    tbe.numPendingDirectoryAtomics := tbe.numPendingDirectoryAtomics - 1;
    if (tbe.numPendingDirectoryAtomics==0) {
      enqueue(triggerQueue_out, TriggerMsg, 1) {
        tbe.atomicDoneCnt := tbe.atomicDoneCnt + 1;
        out_msg.addr := address;
        out_msg.Type := TriggerType:AtomicDone;
        peek(responseFromNB_in, ResponseMsg) {
          out_msg.isGLCSet := in_msg.isGLCSet;
          out_msg.isSLCSet := in_msg.isSLCSet;
        }
      }
    }
  }

  action(dadc_decrementAtomicDoneCnt, "dadc", desc="decrement atomics done cnt flag") {
    tbe.atomicDoneCnt := tbe.atomicDoneCnt - 1;
  }

  action(ptr_popTriggerQueue, "ptr", desc="pop Trigger") {
    triggerQueue_in.dequeue(clockEdge());
  }

  action(pa_performAtomic, "pa", desc="Perform atomic") {
    peek(coreRequestNetwork_in, CPURequestMsg) {
      if (in_msg.Type == CoherenceRequestType:AtomicReturn) {
        cache_entry.DataBlk.atomicPartial(cache_entry.DataBlk, cache_entry.writeMask, false);
      } else {
        // Set the isAtomicNoReturn flag to ensure that logs are not
        // generated erroneously
        assert(in_msg.Type == CoherenceRequestType:AtomicNoReturn);
        cache_entry.DataBlk.atomicPartial(cache_entry.DataBlk, cache_entry.writeMask, true);
      }
    }
  }

  // END ACTIONS

  // BEGIN TRANSITIONS
  // transitions from base
  // Assumptions for ArrayRead/Write
  // TBE checked before tags
  // Data Read/Write requires Tag Read

  // Stalling transitions do NOT check the tag array...and if they do,
  // they can cause a resource stall deadlock!

  transition(WI, {RdBlk, WrVicBlk, Atomic, AtomicPassOn, WrVicBlkBack}) { //TagArrayRead} {
      // don't profile as hit or miss since it will be tried again
      /*
        By putting the stalled requests in a buffer, we reduce resource contention
        since they won't try again every cycle and will instead only try again once
        woken up.
       */
      st_stallAndWaitRequest;
  }
  transition(WIB, {RdBlk, WrVicBlk, Atomic, WrVicBlkBack}) { //TagArrayRead} {
      // don't profile as hit or miss since it will be tried again
      /*
        By putting the stalled requests in a buffer, we reduce resource contention
        since they won't try again every cycle and will instead only try again once
        woken up.
       */
      st_stallAndWaitRequest;
  }
  transition(A, {RdBlk, WrVicBlk, WrVicBlkBack}) { //TagArrayRead} {
      // don't profile as hit or miss since it will be tried again
      /*
        By putting the stalled requests in a buffer, we reduce resource contention
        since they won't try again every cycle and will instead only try again once
        woken up.
       */
      st_stallAndWaitRequest;
  }

  transition(IV, {WrVicBlk, Atomic, AtomicPassOn, WrVicBlkBack}) { //TagArrayRead} {
      // don't profile as hit or miss since it will be tried again
      /*
        By putting the stalled requests in a buffer, we reduce resource contention
        since they won't try again every cycle and will instead only try again once
        woken up.
       */
      st_stallAndWaitRequest;
  }

  transition({I, IV, V, WI}, AtomicWait) {
    // don't profile as hit or miss since it will be tried again
    /*
      By putting the stalled requests in a buffer, we reduce resource contention
      since they won't try again every cycle and will instead only try again once
      woken up.
     */
    st_stallAndWaitRequest;
  }

  transition({M, V}, RdBlk) {TagArrayRead, DataArrayRead} {
    p_profileHit;
    sd_sendData;
    ut_updateTag;
    p_popRequestQueue;
  }

  transition(W, RdBlk, WI) {TagArrayRead, DataArrayRead} {
    // don't profile as hit or miss since it will be tried again
    t_allocateTBE;
    wb_writeBack;
    /*
      Need to try this request again after writing back the current entry -- to
      do so, put it with other stalled requests in a buffer to reduce resource
      contention since they won't try again every cycle and will instead only
      try again once woken up.
     */
    st_stallAndWaitRequest;
  }

  transition(I, RdBlk, IV) {TagArrayRead} {
    p_profileMiss;
    t_allocateTBE;
    rd_requestData;
    p_popRequestQueue;
  }

  transition(IV, RdBlk) {
    p_profileMiss;
    t_allocateTBE;
    rd_requestData;
    p_popRequestQueue;
  }

  transition(I, RdBypassEvict) {TagArrayRead} {
    p_profileMiss;
    t_allocateTBE;
    rd_requestData;
    p_popRequestQueue;
  }

  // Transition to be called when a read request with SLC flag set arrives at
  // entry in state W. It evicts and invalidates the cache entry before
  // forwarding the request to global memory
  transition(W, RdBypassEvict, WIB) {TagArrayRead} {
    p_profileMiss;
    t_allocateTBE;
    wb_writeBack;
    i_invL2;
    rd_requestData;
    p_popRequestQueue;
  }

  // Transition to be called when a read request with SLC flag set arrives at
  // entry in state M. It evicts and invalidates the cache entry before
  // forwarding the request to global memory to main memory
  transition(M, RdBypassEvict, WIB) {TagArrayRead} {
    p_profileMiss;
    t_allocateTBE;
    wb_writeBack;
    i_invL2;
    rd_requestData;
    p_popRequestQueue;
  }

  // Transition to be called when a read request with SLC flag set arrives at
  // entry in state V. It invalidates the cache entry before forwarding the
  // request to global memory.
  transition(V, RdBypassEvict, I) {TagArrayRead} {
    p_profileMiss;
    t_allocateTBE;
    i_invL2;
    rd_requestData;
    p_popRequestQueue;
  }

  // Transition to be called when a read request with SLC flag arrives at entry
  // in transient state. The request stalls until the pending transition is complete.
  transition({WI, WIB, IV}, RdBypassEvict)  {
    // don't profile as hit or miss since it will be tried again
    st_stallAndWaitRequest;
  }

  transition(V, Atomic, M) {TagArrayRead, TagArrayWrite, DataArrayWrite, AtomicALUOperation} {
    p_profileHit;
    ut_updateTag;
    owm_orWriteMask;
    pa_performAtomic;
    ar_sendAtomicResponse;
    p_popRequestQueue;
  }

  transition(A, {Atomic, AtomicWait}) {
    // don't profile as hit or miss since it will be tried again
    // by putting the stalled requests in a buffer, we reduce resource contention
    // since they won't try again every cycle and will instead only try again once
    // woken up
    st_stallAndWaitRequest;
  }

  transition(W, Atomic, WI) {
    t_allocateTBE;
    wb_writeBack;
    // need to try this request again after writing back the current entry -- to
    // do so, put it with other stalled requests in a buffer to reduce resource
    // contention since they won't try again every cycle and will instead only
    // try again once woken up
    st_stallAndWaitRequest;
  }

  transition(M, Atomic) {TagArrayRead, DataArrayWrite, AtomicALUOperation} {
    p_profileHit;
    owm_orWriteMask;
    pa_performAtomic;
    ar_sendAtomicResponse;
    p_popRequestQueue;
  }

  // The following atomic pass on actions will send the request to the directory,
  // and are triggered when an atomic request is received that is not in TCC,
  // and/or if SLC is set.
  transition(V, AtomicPassOn, A) {TagArrayRead} {
    p_profileHit;
    i_invL2;
    t_allocateTBE;
    at_atomicThrough;
    inpa_incrementNumPendingDirectoryAtomics;
    p_popRequestQueue;
  }

  transition(I, {Atomic, AtomicPassOn}, A) {TagArrayRead} {
    p_profileMiss;
    i_invL2;
    t_allocateTBE;
    at_atomicThrough;
    inpa_incrementNumPendingDirectoryAtomics;
    p_popRequestQueue;
  }

  transition(A, AtomicPassOn) {
    // don't profile as hit or miss since it will be tried again
    // by putting the stalled requests in a buffer, we reduce resource contention
    // since they won't try again every cycle and will instead only try again once
    // woken up
    st_stallAndWaitRequest;
  }

  transition({M, W}, AtomicPassOn, WI) {TagArrayRead, DataArrayRead} {
    t_allocateTBE;
    wb_writeBack;
    // after writing back the current line, we need to wait for it to be done
    // before we try to perform the atomic
    // by putting the stalled requests in a buffer, we reduce resource contention
    // since they won't try again every cycle and will instead only try again once
    // woken up
    st_stallAndWaitRequest;
  }

  transition(I, WrVicBlk) {TagArrayRead} {
    p_profileMiss;
    wt_writeThrough;
    p_popRequestQueue;
  }

  transition(V, WrVicBlk) {TagArrayRead, DataArrayWrite} {
    p_profileHit;
    ut_updateTag;
    wdb_writeDirtyBytes;
    wt_writeThrough;
    p_popRequestQueue;
  }

  transition({V, M}, WrVicBlkBack, M) {TagArrayRead, TagArrayWrite, DataArrayWrite} {
    p_profileHit;
    ut_updateTag;
    swb_sendWBAck;
    wdb_writeDirtyBytes;
    p_popRequestQueue;
  }

  transition(W, WrVicBlkBack) {TagArrayRead, TagArrayWrite, DataArrayWrite} {
    p_profileHit;
    ut_updateTag;
    swb_sendWBAck;
    wdb_writeDirtyBytes;
    p_popRequestQueue;
  }

  transition(I, WrVicBlkBack, W) {TagArrayRead, TagArrayWrite, DataArrayWrite} {
    p_profileMiss;
    a_allocateBlock;
    ut_updateTag;
    swb_sendWBAck;
    wdb_writeDirtyBytes;
    p_popRequestQueue;
  }

  // Transition to be called when a write request with SLC bit set arrives at an
  // entry with state V. The entry has to be evicted and invalidated before the
  // request is forwarded to global memory
  transition(V, WrVicBlkEvict, I) {TagArrayRead, TagArrayWrite, DataArrayWrite} {
    p_profileMiss;
    ut_updateTag;
    t_allocateTBE;
    wt_writeThrough;
    i_invL2;
    p_popRequestQueue;
  }

  // Transition to be called when a write request with SLC bit set arrives at an
  // entry with state W. The entry has to be evicted and invalidated before the
  // request is forwarded to global memory.
  transition(W, WrVicBlkEvict, I) {TagArrayRead, TagArrayWrite, DataArrayWrite} {
    p_profileMiss;
    ut_updateTag;
    wdb_writeDirtyBytes;
    t_allocateTBE;
    wb_writeBack;
    i_invL2;
    p_popRequestQueue;
  }

  transition({W, M}, L2_Repl, WI) {TagArrayRead, DataArrayRead} {
    t_allocateTBE;
    wb_writeBack;
    i_invL2;
  }

  transition({I, V}, L2_Repl, I) {TagArrayRead, TagArrayWrite} {
    i_invL2;
  }

  transition({A, IV, WI, WIB}, L2_Repl) {
    i_invL2;
  }

  transition({I, V}, PrbInv, I) {TagArrayRead, TagArrayWrite} {
    pi_sendProbeResponseInv;
    pp_popProbeQueue;
  }

  transition(M, PrbInv, W) {TagArrayRead, TagArrayWrite} {
    pi_sendProbeResponseInv;
    pp_popProbeQueue;
  }

  transition(W, PrbInv) {TagArrayRead} {
    pi_sendProbeResponseInv;
    pp_popProbeQueue;
  }

  transition({A, IV, WI, WIB}, PrbInv) {
    pi_sendProbeResponseInv;
    pp_popProbeQueue;
  }

  // Transition to be called when the response for a request with SLC bit set
  // arrives. The request has to be forwarded to the core that needs it while
  // making sure no entry is allocated.
  transition(I, Bypass, I) {
    rb_bypassDone;
    pr_popResponseQueue;
    wada_wakeUpAllDependentsAddr;
    dt_deallocateTBE;
  }

  transition(A, Bypass) {TagArrayRead, TagArrayWrite} {
    bapdr_sendBypassedAtomicPerformedInDirectoryResponse;
    dnpa_decrementNumPendingDirectoryAtomics;
    pr_popResponseQueue;
  }

  transition(WI, Bypass, I) {
    pr_popResponseQueue;
    wada_wakeUpAllDependentsAddr;
    dt_deallocateTBE;
  }

  transition(IV, Data, V) {TagArrayRead, TagArrayWrite, DataArrayWrite} {
    a_allocateBlock;
    ut_updateTag;
    wcb_writeCacheBlock;
    sdr_sendDataResponse;
    wada_wakeUpAllDependentsAddr;
    dt_deallocateTBE;
    pr_popResponseQueue;
  }

  /*
    Since the L2 now allows multiple loads from different CUs to proceed in
    parallel to the directory, we may get Event:Data back when the line is
    already in V.  In this case, send the response to the appropriate TCP
    and update MRU/data in TCC, but don't need to allocate line.
   */
  transition(V, Data) {TagArrayRead, TagArrayWrite, DataArrayWrite} {
    ut_updateTag;
    wcb_writeCacheBlock;
    sdr_sendDataResponse;
    wada_wakeUpAllDependentsAddr;
    // tracks # pending requests, so need to decrement here too
    dt_deallocateTBE;
    pr_popResponseQueue;
  }

  /*
    Since the L2 now allows multiple loads from different CUs to proceed in
    parallel to the directory, we may get Event:Data back when the line is
    now in I because it has been evicted by an intervening request to the same
    set index.  In this case, send the response to the appropriate TCP without
    affecting the TCC (essentially, treat it similar to a bypass request except
    we also send the unblock back to the directory).
   */
  transition(I, Data) {
    sdr_sendDataResponse;
    wada_wakeUpAllDependentsAddr;
    // tracks # pending requests, so need to decrement here too
    dt_deallocateTBE;
    pr_popResponseQueue;
  }

  transition(A, Data, M) {TagArrayRead, TagArrayWrite, DataArrayWrite, AtomicALUOperation} {
    a_allocateBlock;
    wardb_writeAtomicResponseDirtyBytes;
    pa_performAtomic;
    baplr_sendBypassedAtomicPerformedLocallyResponse;
    dt_deallocateTBE;
    pr_popResponseQueue;
  }

  transition(A, AtomicDone, I) {TagArrayRead, TagArrayWrite} {
    dt_deallocateTBE;
    wada_wakeUpAllDependentsAddr;
    ptr_popTriggerQueue;
  }

  transition(A, AtomicNotDone) {TagArrayRead} {
    dadc_decrementAtomicDoneCnt;
    ptr_popTriggerQueue;
  }

  //M,W should not see WBAck as the cache is in WB mode
  //WBAcks do not need to check tags
  transition({I, V, IV, A}, WBAck) {
    w_sendResponseWBAck;
    pr_popResponseQueue;
  }

  transition(WI, WBAck,I) {
    dt_deallocateTBE;
    wada_wakeUpAllDependentsAddr;
    pr_popResponseQueue;
  }

  transition(WIB, WBAck,I) {
    pr_popResponseQueue;
  }

  transition({A, IV, WI, WIB}, Flush) {
    st_stallAndWaitRequest;
  }

  transition(I, Flush) {
    fw_sendFlushResponse;
    p_popRequestQueue;
  }

  transition({V, W}, Flush, I) {TagArrayRead, TagArrayWrite} {
    t_allocateTBE;
    ut_updateTag;
    f_flush;
    i_invL2;
    p_popRequestQueue;
   }
}
