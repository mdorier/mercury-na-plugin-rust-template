use std::collections::{HashMap, VecDeque};
use std::ffi::c_void;
use std::os::unix::io::RawFd;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;

use libp2p::{Multiaddr, PeerId};
use parking_lot::Mutex;
use tokio::sync::mpsc;

use crate::bindings::*;
use crate::runtime::{Command, CompletionItem};

/// Maximum message size for both unexpected and expected messages (64 KB).
pub const MAX_MSG_SIZE: usize = 64 * 1024;

// ---------------------------------------------------------------------------
//  Transport configuration
// ---------------------------------------------------------------------------

/// Which transports are enabled, and which is the priority (displayed by default).
#[derive(Debug, Clone)]
pub struct TransportConfig {
    pub tcp: bool,
    pub quic: bool,
    pub relay: bool,
    pub priority: TransportKind,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TransportKind {
    Tcp,
    Quic,
}

// ---------------------------------------------------------------------------
//  Plugin-level state (stored in na_class.plugin_class)
// ---------------------------------------------------------------------------

pub struct NaLibp2pClass {
    pub self_addr: Arc<NaLibp2pAddr>,
    /// Shared with the async runtime for incoming message matching.
    pub queues: Arc<Mutex<OperationQueues>>,
    pub cmd_tx: mpsc::UnboundedSender<Command>,
    pub completion_rx: Mutex<mpsc::UnboundedReceiver<CompletionItem>>,
    pub event_fd: RawFd,
    pub runtime: Option<tokio::runtime::Runtime>,
    pub swarm_join: Option<tokio::task::JoinHandle<()>>,
    /// Shared with the async runtime for RMA operations.
    pub mem_handles: Arc<Mutex<HashMap<u64, MemHandleEntry>>>,
    pub next_mem_handle_id: AtomicU64,
    /// Transport configuration (TCP, QUIC, or both).
    pub transport_config: TransportConfig,
    /// All resolved listen multiaddrs (TCP and/or QUIC) — used by addr_to_string hints.
    pub listen_addrs: Vec<Multiaddr>,
    /// Relay server address (when relay transport is enabled).
    pub relay_addr: Option<Multiaddr>,
}

impl NaLibp2pClass {
    pub fn next_handle_id(&self) -> u64 {
        self.next_mem_handle_id.fetch_add(1, Ordering::Relaxed)
    }
}

/// Queues for pending operations and stashed (early-arriving) messages.
pub struct OperationQueues {
    pub unexpected_recv_ops: VecDeque<*mut NaLibp2pOpId>,
    pub expected_recv_ops: VecDeque<*mut NaLibp2pOpId>,
    pub unexpected_msg_stash: VecDeque<StashedMessage>,
    pub expected_msg_stash: VecDeque<StashedMessage>,
    pub pending_rma_ops: VecDeque<*mut NaLibp2pOpId>,
}

unsafe impl Send for OperationQueues {}

impl OperationQueues {
    pub fn new() -> Self {
        Self {
            unexpected_recv_ops: VecDeque::new(),
            expected_recv_ops: VecDeque::new(),
            unexpected_msg_stash: VecDeque::new(),
            expected_msg_stash: VecDeque::new(),
            pending_rma_ops: VecDeque::new(),
        }
    }
}

/// A message that arrived before a matching recv was posted.
pub struct StashedMessage {
    pub payload: Vec<u8>,
    pub source_addr: *mut NaLibp2pAddr,
    pub tag: u32,
}

unsafe impl Send for StashedMessage {}

/// Entry in the local memory handle registry.
pub struct MemHandleEntry {
    pub buf: *mut c_void,
    pub buf_size: usize,
    pub flags: u64,
}

unsafe impl Send for MemHandleEntry {}

// ---------------------------------------------------------------------------
//  Address
// ---------------------------------------------------------------------------

pub struct NaLibp2pAddr {
    pub peer_id: PeerId,
    /// Transport multiaddr (without the trailing /p2p/<peer_id> component).
    /// None for peers discovered only via incoming connections.
    pub multiaddr: Option<Multiaddr>,
    pub is_self: bool,
}

impl NaLibp2pAddr {
    /// Format: `<transport>:<multiaddr>/p2p/<peer-id>`
    /// e.g.   `tcp:/ip4/192.168.1.10/tcp/43210/p2p/12D3KooW...`
    /// or     `quic:/ip4/192.168.1.10/udp/43210/quic-v1/p2p/12D3KooW...`
    ///
    /// Mercury prepends `libp2p+` automatically, giving:
    /// `libp2p+tcp:/ip4/.../tcp/.../p2p/...`
    pub fn to_addr_string(&self) -> String {
        match &self.multiaddr {
            Some(ma) => {
                let prefix = transport_prefix_for_multiaddr(ma);
                format!("{prefix}:{}/p2p/{}", ma, self.peer_id)
            }
            None => format!("tcp:/p2p/{}", self.peer_id),
        }
    }

    pub fn alloc_boxed(
        peer_id: PeerId,
        multiaddr: Option<Multiaddr>,
        is_self: bool,
    ) -> *mut Self {
        Box::into_raw(Box::new(Self {
            peer_id,
            multiaddr,
            is_self,
        }))
    }

    pub fn dup(src: &NaLibp2pAddr) -> *mut Self {
        Box::into_raw(Box::new(NaLibp2pAddr {
            peer_id: src.peer_id,
            multiaddr: src.multiaddr.clone(),
            is_self: src.is_self,
        }))
    }
}

// ---------------------------------------------------------------------------
//  Operation ID
// ---------------------------------------------------------------------------

#[repr(C)]
pub struct NaLibp2pOpId {
    pub completion_data: na_cb_completion_data,
    pub context: *mut na_context_t,
    pub addr: *mut NaLibp2pAddr,
    pub tag: na_tag_t,
    pub buf: *mut c_void,
    pub buf_size: usize,
    pub local_handle_id: u64,
    pub local_offset: u64,
    pub rma_length: usize,
}

unsafe impl Send for NaLibp2pOpId {}

/// Plugin release callback — called after the user callback returns.
pub unsafe extern "C" fn na_libp2p_release(_arg: *mut c_void) {
    // Nothing to do — op lifetime is managed by op_create/op_destroy.
}

// ---------------------------------------------------------------------------
//  Memory Handle
// ---------------------------------------------------------------------------

pub struct NaLibp2pMemHandle {
    pub buf: *mut c_void,
    pub buf_size: usize,
    pub handle_id: u64,
    pub flags: u64,
    pub owner_peer_id: Option<PeerId>,
}

// ---------------------------------------------------------------------------
//  Helpers
// ---------------------------------------------------------------------------

/// Determine transport prefix ("tcp" or "quic") from a multiaddr's content.
pub fn transport_prefix_for_multiaddr(ma: &Multiaddr) -> &'static str {
    for proto in ma.iter() {
        if matches!(proto, libp2p::multiaddr::Protocol::P2pCircuit) {
            return "relay";
        }
    }
    for proto in ma.iter() {
        if matches!(proto, libp2p::multiaddr::Protocol::QuicV1) {
            return "quic";
        }
    }
    "tcp"
}
