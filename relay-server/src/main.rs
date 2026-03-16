use std::error::Error;
use std::net::Ipv4Addr;

use futures::StreamExt;
use libp2p::{
    core::multiaddr::Protocol, identify, identity, noise, ping, relay,
    swarm::{NetworkBehaviour, SwarmEvent},
    tcp, yamux, Multiaddr,
};
use tracing_subscriber::EnvFilter;

#[derive(NetworkBehaviour)]
struct Behaviour {
    relay: relay::Behaviour,
    ping: ping::Behaviour,
    identify: identify::Behaviour,
}

fn generate_ed25519(seed: u8) -> identity::Keypair {
    let mut bytes = [0u8; 32];
    bytes[0] = seed;
    identity::Keypair::ed25519_from_bytes(bytes).expect("only errors on wrong length")
}

fn parse_args() -> (u16, u8, String) {
    let args: Vec<String> = std::env::args().collect();
    let mut port: u16 = 0;
    let mut seed: Option<u8> = None;
    let mut addr_file: Option<String> = None;

    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "--port" => {
                i += 1;
                port = args[i].parse().expect("invalid port");
            }
            "--secret-key-seed" => {
                i += 1;
                seed = Some(args[i].parse().expect("invalid seed"));
            }
            "--addr-file" => {
                i += 1;
                addr_file = Some(args[i].clone());
            }
            other => {
                eprintln!("Unknown argument: {other}");
                std::process::exit(1);
            }
        }
        i += 1;
    }

    let seed = seed.expect("--secret-key-seed is required");
    let addr_file = addr_file.expect("--addr-file is required");
    (port, seed, addr_file)
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn Error>> {
    let _ = tracing_subscriber::fmt()
        .with_env_filter(EnvFilter::from_default_env())
        .try_init();

    let (port, seed, addr_file) = parse_args();

    let local_key = generate_ed25519(seed);
    let local_peer_id = local_key.public().to_peer_id();

    println!("Relay server PeerId: {local_peer_id}");

    let mut swarm = libp2p::SwarmBuilder::with_existing_identity(local_key)
        .with_tokio()
        .with_tcp(
            tcp::Config::default(),
            noise::Config::new,
            yamux::Config::default,
        )?
        .with_behaviour(|key| Behaviour {
            relay: relay::Behaviour::new(key.public().to_peer_id(), Default::default()),
            ping: ping::Behaviour::new(ping::Config::new()),
            identify: identify::Behaviour::new(identify::Config::new(
                "/mercury-relay-test/0.1.0".to_string(),
                key.public(),
            )),
        })?
        .build();

    let listen_addr = Multiaddr::empty()
        .with(Protocol::from(Ipv4Addr::UNSPECIFIED))
        .with(Protocol::Tcp(port));
    swarm.listen_on(listen_addr)?;

    let mut addr_written = false;

    loop {
        tokio::select! {
            event = swarm.next() => {
                match event.expect("swarm stream ended") {
                    SwarmEvent::NewListenAddr { address, .. } => {
                        // Resolve 0.0.0.0 to 127.0.0.1 for local tests
                        let resolved: Multiaddr = address
                            .iter()
                            .map(|p| match p {
                                Protocol::Ip4(ip) if ip.is_unspecified() => {
                                    Protocol::Ip4(Ipv4Addr::LOCALHOST)
                                }
                                other => other,
                            })
                            .collect();

                        // Add resolved address as external so relay
                        // reservations include it in the response.
                        swarm.add_external_address(resolved.clone());

                        let full_addr = format!("{resolved}/p2p/{local_peer_id}");
                        println!("Relay listening on {full_addr}");

                        if !addr_written {
                            std::fs::write(&addr_file, &full_addr)
                                .expect("failed to write addr file");
                            addr_written = true;
                        }
                    }
                    SwarmEvent::Behaviour(BehaviourEvent::Identify(
                        identify::Event::Received { info, .. },
                    )) => {
                        swarm.add_external_address(info.observed_addr);
                    }
                    SwarmEvent::Behaviour(BehaviourEvent::Relay(event)) => {
                        println!("Relay event: {event:?}");
                    }
                    _ => {}
                }
            }
            _ = tokio::signal::ctrl_c() => {
                println!("Relay server shutting down");
                break;
            }
        }
    }

    Ok(())
}
