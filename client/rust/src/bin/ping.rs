use std::time::Duration;

use rsp_client::RspClient;

#[tokio::main(flavor = "current_thread")]
async fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 3 {
        eprintln!("Usage: cargo run --bin ping -- tcp:<host>:<port> <destination-node-id>");
        std::process::exit(2);
    }

    let transport_spec = &args[1];
    let destination_node_id = &args[2];

    let mut client = match RspClient::new() {
        Ok(client) => client,
        Err(error) => {
            eprintln!("failed to initialize client: {error}");
            std::process::exit(1);
        }
    };

    if let Err(error) = client.connect(transport_spec).await {
        eprintln!("failed to connect: {error}");
        std::process::exit(1);
    }

    let ping_result = client.ping(destination_node_id, Duration::from_secs(5)).await;
    let peer_node_id = client.peer_node_id().await;
    let close_result = client.close().await;

    match ping_result {
        Ok(success) => {
            println!(
                "{}",
                serde_json::json!({
                    "local_node_id": client.node_id(),
                    "resource_manager_node_id": peer_node_id,
                    "destination_node_id": destination_node_id,
                    "success": success,
                })
            );
            if !success {
                std::process::exit(1);
            }
        }
        Err(error) => {
            eprintln!("ping failed: {error}");
            std::process::exit(1);
        }
    }

    if let Err(error) = close_result {
        eprintln!("close failed: {error}");
        std::process::exit(1);
    }
}
