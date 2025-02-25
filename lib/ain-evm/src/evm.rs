use crate::backend::{EVMBackend, Vicinity};
use crate::block::BlockService;
use crate::core::{EVMCoreService, EVMError, NativeTxHash, MAX_GAS_PER_BLOCK};
use crate::executor::{AinExecutor, TxResponse};
use crate::fee::{calculate_gas_fee, calculate_prepay_gas_fee, get_tx_max_gas_price};
use crate::filters::FilterService;
use crate::log::LogService;
use crate::receipt::ReceiptService;
use crate::storage::traits::BlockStorage;
use crate::storage::Storage;
use crate::traits::Executor;
use crate::transaction::SignedTx;
use crate::trie::GENESIS_STATE_ROOT;
use crate::txqueue::QueueTx;

use ethereum::{Block, PartialHeader, ReceiptV3, TransactionV2};
use ethereum_types::{Bloom, H160, H64, U256};

use crate::bytes::Bytes;
use crate::services::SERVICES;
use crate::transaction::system::{BalanceUpdate, DST20Data, DeployContractData, SystemTx};
use ain_contracts::{Contracts, CONTRACT_ADDRESSES};
use anyhow::anyhow;
use hex::FromHex;
use log::debug;
use primitive_types::H256;
use std::error::Error;
use std::path::PathBuf;
use std::sync::Arc;

pub struct EVMServices {
    pub core: EVMCoreService,
    pub block: BlockService,
    pub receipt: ReceiptService,
    pub logs: LogService,
    pub filters: FilterService,
    pub storage: Arc<Storage>,
}

pub struct FinalizedBlockInfo {
    pub block_hash: [u8; 32],
    pub failed_transactions: Vec<String>,
    pub total_burnt_fees: U256,
    pub total_priority_fees: U256,
}

pub struct DeployContractInfo {
    pub address: H160,
    pub storage: Vec<(H256, H256)>,
    pub bytecode: Bytes,
}

pub struct DST20BridgeInfo {
    pub address: H160,
    pub storage: Vec<(H256, H256)>,
}

impl EVMServices {
    /// Constructs a new Handlers instance. Depending on whether the defid -ethstartstate flag is set,
    /// it either revives the storage from a previously saved state or initializes new storage using input from a JSON file.
    /// This JSON-based initialization is exclusively reserved for regtest environments.
    ///
    /// # Warning
    ///
    /// Loading state from JSON will overwrite previous stored state
    ///
    /// # Errors
    ///
    /// This method will return an error if an attempt is made to load a genesis state from a JSON file outside of a regtest environment.
    ///
    /// # Return
    ///
    /// Returns an instance of the struct, either restored from storage or created from a JSON file.
    pub fn new() -> Result<Self, anyhow::Error> {
        if let Some(path) = ain_cpp_imports::get_state_input_json() {
            if ain_cpp_imports::get_network() != "regtest" {
                return Err(anyhow!(
                    "Loading a genesis from JSON file is restricted to regtest network"
                ));
            }
            let storage = Arc::new(Storage::new());
            Ok(Self {
                core: EVMCoreService::new_from_json(Arc::clone(&storage), PathBuf::from(path)),
                block: BlockService::new(Arc::clone(&storage)),
                receipt: ReceiptService::new(Arc::clone(&storage)),
                logs: LogService::new(Arc::clone(&storage)),
                filters: FilterService::new(),
                storage,
            })
        } else {
            let storage = Arc::new(Storage::restore());
            Ok(Self {
                core: EVMCoreService::restore(Arc::clone(&storage)),
                block: BlockService::new(Arc::clone(&storage)),
                receipt: ReceiptService::new(Arc::clone(&storage)),
                logs: LogService::new(Arc::clone(&storage)),
                filters: FilterService::new(),
                storage,
            })
        }
    }

    pub fn finalize_block(
        &self,
        queue_id: u64,
        update_state: bool,
        difficulty: u32,
        beneficiary: H160,
        timestamp: u64,
    ) -> Result<FinalizedBlockInfo, Box<dyn Error>> {
        let mut all_transactions = Vec::with_capacity(self.core.tx_queues.len(queue_id));
        let mut failed_transactions = Vec::with_capacity(self.core.tx_queues.len(queue_id));
        let mut receipts_v3: Vec<ReceiptV3> = Vec::with_capacity(self.core.tx_queues.len(queue_id));
        let mut total_gas_used = 0u64;
        let mut total_gas_fees = U256::zero();
        let mut logs_bloom: Bloom = Bloom::default();

        let parent_data = self.block.get_latest_block_hash_and_number();
        let state_root = self
            .storage
            .get_latest_block()
            .map_or(GENESIS_STATE_ROOT.parse().unwrap(), |block| {
                block.header.state_root
            });

        let (vicinity, parent_hash, current_block_number) = match parent_data {
            None => (
                Vicinity {
                    beneficiary,
                    timestamp: U256::from(timestamp),
                    block_number: U256::zero(),
                    ..Default::default()
                },
                H256::zero(),
                U256::zero(),
            ),
            Some((hash, number)) => (
                Vicinity {
                    beneficiary,
                    timestamp: U256::from(timestamp),
                    block_number: number + 1,
                    ..Default::default()
                },
                hash,
                number + 1,
            ),
        };

        let base_fee = self.block.calculate_base_fee(parent_hash);
        debug!("[finalize_block] Block base fee: {}", base_fee);

        let mut backend = EVMBackend::from_root(
            state_root,
            Arc::clone(&self.core.trie_store),
            Arc::clone(&self.storage),
            vicinity,
        )?;

        let mut executor = AinExecutor::new(&mut backend);

        // Ensure that state root changes by updating counter contract storage
        if current_block_number == U256::zero() {
            // Deploy contract on the first block
            let DeployContractInfo {
                address,
                storage,
                bytecode,
            } = EVMServices::counter_contract()?;
            executor.deploy_contract(address, bytecode, storage)?;
        } else {
            let DeployContractInfo {
                address, storage, ..
            } = EVMServices::counter_contract()?;
            executor.update_storage(address, storage)?;
        }

        for queue_item in self.core.tx_queues.get_cloned_vec(queue_id) {
            match queue_item.queue_tx {
                QueueTx::SignedTx(signed_tx) => {
                    let nonce = executor.get_nonce(&signed_tx.sender);
                    if signed_tx.nonce() != nonce {
                        return Err(anyhow!("EVM block rejected for invalid nonce. Address {} nonce {}, signed_tx nonce: {}", signed_tx.sender, nonce, signed_tx.nonce()).into());
                    }

                    let prepay_gas = calculate_prepay_gas_fee(&signed_tx)?;
                    let (
                        TxResponse {
                            exit_reason,
                            logs,
                            used_gas,
                            ..
                        },
                        receipt,
                    ) = executor.exec(&signed_tx, prepay_gas);
                    debug!(
                        "receipt : {:#?} for signed_tx : {:#x}",
                        receipt,
                        signed_tx.transaction.hash()
                    );

                    if !exit_reason.is_succeed() {
                        failed_transactions.push(hex::encode(queue_item.tx_hash));
                    }

                    let gas_fee = calculate_gas_fee(&signed_tx, U256::from(used_gas), base_fee)?;
                    total_gas_used += used_gas;
                    total_gas_fees += gas_fee;

                    all_transactions.push(signed_tx.clone());
                    EVMCoreService::logs_bloom(logs, &mut logs_bloom);
                    receipts_v3.push(receipt);
                }
                QueueTx::SystemTx(SystemTx::EvmIn(BalanceUpdate { address, amount })) => {
                    debug!(
                        "[finalize_block] EvmIn for address {:x?}, amount: {}, queue_id {}",
                        address, amount, queue_id
                    );
                    if let Err(e) = executor.add_balance(address, amount) {
                        debug!("[finalize_block] EvmIn failed with {e}");
                        failed_transactions.push(hex::encode(queue_item.tx_hash));
                    }
                }
                QueueTx::SystemTx(SystemTx::EvmOut(BalanceUpdate { address, amount })) => {
                    debug!(
                        "[finalize_block] EvmOut for address {}, amount: {}",
                        address, amount
                    );

                    if let Err(e) = executor.sub_balance(address, amount) {
                        debug!("[finalize_block] EvmOut failed with {e}");
                        failed_transactions.push(hex::encode(queue_item.tx_hash));
                    }
                }
                QueueTx::SystemTx(SystemTx::DeployContract(DeployContractData {
                    name,
                    symbol,
                    address,
                })) => {
                    debug!(
                        "[finalize_block] DeployContract for address {}, name {}, symbol {}",
                        address, name, symbol
                    );

                    let DeployContractInfo {
                        address,
                        bytecode,
                        storage,
                    } = EVMServices::dst20_contract(&mut executor, address, name, symbol)?;

                    if let Err(e) = executor.deploy_contract(address, bytecode, storage) {
                        debug!("[finalize_block] EvmOut failed with {e}");
                    }
                }
                QueueTx::SystemTx(SystemTx::DST20Bridge(DST20Data {
                    to,
                    contract,
                    amount,
                    out,
                })) => {
                    debug!(
                        "[finalize_block] DST20Bridge for to {}, contract {}, amount {}, out {}",
                        to, contract, amount, out
                    );

                    match EVMServices::bridge_dst20(&mut executor, contract, to, amount, out) {
                        Ok(DST20BridgeInfo { address, storage }) => {
                            if let Err(e) = executor.update_storage(address, storage) {
                                debug!("[finalize_block] EvmOut failed with {e}");
                                failed_transactions.push(hex::encode(queue_item.tx_hash));
                            }
                        }
                        Err(e) => {
                            debug!("[finalize_block] EvmOut failed with {e}");
                            failed_transactions.push(hex::encode(queue_item.tx_hash));
                        }
                    }
                }
            }

            executor.commit();
        }

        let block = Block::new(
            PartialHeader {
                parent_hash,
                beneficiary,
                state_root: if update_state {
                    backend.commit()
                } else {
                    backend.root()
                },
                receipts_root: ReceiptService::get_receipts_root(&receipts_v3),
                logs_bloom,
                difficulty: U256::from(difficulty),
                number: current_block_number,
                gas_limit: MAX_GAS_PER_BLOCK,
                gas_used: U256::from(total_gas_used),
                timestamp,
                extra_data: Vec::default(),
                mix_hash: H256::default(),
                nonce: H64::default(),
                base_fee,
            },
            all_transactions
                .iter()
                .map(|signed_tx| signed_tx.transaction.clone())
                .collect(),
            Vec::new(),
        );

        let receipts = self.receipt.generate_receipts(
            &all_transactions,
            receipts_v3,
            block.header.hash(),
            block.header.number,
        );

        if update_state {
            debug!(
                "[finalize_block] Finalizing block number {:#x}, state_root {:#x}",
                block.header.number, block.header.state_root
            );

            self.block.connect_block(block.clone());
            self.logs
                .generate_logs_from_receipts(&receipts, block.header.number);
            self.receipt.put_receipts(receipts);
            self.filters.add_block_to_filters(block.header.hash());
        }

        let total_burnt_fees = U256::from(total_gas_used) * base_fee;
        let total_priority_fees = total_gas_fees - total_burnt_fees;
        debug!(
            "[finalize_block] Total burnt fees : {:#?}",
            total_burnt_fees
        );
        debug!(
            "[finalize_block] Total priority fees : {:#?}",
            total_priority_fees
        );

        match self.core.tx_queues.get_total_fees(queue_id) {
            Some(total_fees) => {
                if (total_burnt_fees + total_priority_fees) != total_fees {
                    return Err(anyhow!("EVM block rejected because block total fees != (burnt fees + priority fees). Burnt fees: {}, priority fees: {}, total fees: {}", total_burnt_fees, total_priority_fees, total_fees).into());
                }
            }
            None => {
                return Err(anyhow!(
                    "EVM block rejected because failed to get total fees from queue_id: {}",
                    queue_id
                )
                .into())
            }
        }

        if update_state {
            self.core.tx_queues.remove(queue_id);
        }

        Ok(FinalizedBlockInfo {
            block_hash: *block.header.hash().as_fixed_bytes(),
            failed_transactions,
            total_burnt_fees,
            total_priority_fees,
        })
    }

    pub fn verify_tx_fees(&self, tx: &str, use_context: bool) -> Result<(), Box<dyn Error>> {
        debug!("[verify_tx_fees] raw transaction : {:#?}", tx);
        let buffer = <Vec<u8>>::from_hex(tx)?;
        let tx: TransactionV2 = ethereum::EnvelopedDecodable::decode(&buffer)
            .map_err(|_| anyhow!("Error: decoding raw tx to TransactionV2"))?;
        debug!("[verify_tx_fees] TransactionV2 : {:#?}", tx);
        let signed_tx: SignedTx = tx.try_into()?;

        let mut block_fee = self.block.calculate_base_fee(H256::zero());
        if use_context {
            block_fee = self.block.calculate_next_block_base_fee();
        }

        let tx_gas_price = get_tx_max_gas_price(&signed_tx);
        if tx_gas_price < block_fee {
            debug!("[verify_tx_fees] tx gas price is lower than block base fee");
            return Err(anyhow!("tx gas price is lower than block base fee").into());
        }

        Ok(())
    }

    pub fn queue_tx(
        &self,
        queue_id: u64,
        tx: QueueTx,
        hash: NativeTxHash,
        gas_used: U256,
    ) -> Result<(), EVMError> {
        let parent_data = self.block.get_latest_block_hash_and_number();
        let parent_hash = match parent_data {
            Some((hash, _)) => hash,
            None => H256::zero(),
        };
        let base_fee = self.block.calculate_base_fee(parent_hash);

        self.core
            .tx_queues
            .queue_tx(queue_id, tx.clone(), hash, gas_used, base_fee)?;

        if let QueueTx::SignedTx(signed_tx) = tx {
            self.filters.add_tx_to_filters(signed_tx.transaction.hash())
        }

        Ok(())
    }

    /// Returns address, bytecode and storage with incremented count for the counter contract
    pub fn counter_contract() -> Result<DeployContractInfo, Box<dyn Error>> {
        let address = *CONTRACT_ADDRESSES.get(&Contracts::CounterContract).unwrap();
        let bytecode = ain_contracts::get_counter_bytecode()?;
        let count = SERVICES
            .evm
            .core
            .get_latest_contract_storage(address, ain_contracts::u256_to_h256(U256::one()))?;

        debug!("Count: {:#x}", count + U256::one());

        Ok(DeployContractInfo {
            address,
            bytecode: Bytes::from(bytecode),
            storage: vec![(
                H256::from_low_u64_be(1),
                ain_contracts::u256_to_h256(count + U256::one()),
            )],
        })
    }

    pub fn dst20_contract(
        executor: &mut AinExecutor,
        address: H160,
        name: String,
        symbol: String,
    ) -> Result<DeployContractInfo, Box<dyn Error>> {
        if executor.backend.get_account(&address).is_some() {
            return Err(anyhow!("Token address is already in use").into());
        }

        let bytecode = ain_contracts::get_dst20_bytecode()?;
        let storage = vec![
            (
                H256::from_low_u64_be(3),
                ain_contracts::get_abi_encoded_string(name.as_str()),
            ),
            (
                H256::from_low_u64_be(4),
                ain_contracts::get_abi_encoded_string(symbol.as_str()),
            ),
        ];

        Ok(DeployContractInfo {
            address,
            bytecode: Bytes::from(bytecode),
            storage,
        })
    }

    pub fn bridge_dst20(
        executor: &mut AinExecutor,
        contract: H160,
        to: H160,
        amount: U256,
        out: bool,
    ) -> Result<DST20BridgeInfo, Box<dyn Error>> {
        // check if code of address matches DST20 bytecode
        let account = executor
            .backend
            .get_account(&contract)
            .ok_or_else(|| anyhow!("DST20 token address is not a contract"))?;

        if account.code_hash != ain_contracts::get_dst20_codehash()? {
            return Err(anyhow!("DST20 token code is not valid").into());
        }

        let storage_index = ain_contracts::get_address_storage_index(to);
        let balance = executor
            .backend
            .get_contract_storage(contract, storage_index.as_bytes())?;

        let new_balance = match out {
            true => balance.checked_sub(amount),
            false => balance.checked_add(amount),
        }
        .ok_or_else(|| anyhow!("Balance overflow/underflow"))?;

        Ok(DST20BridgeInfo {
            address: contract,
            storage: vec![(storage_index, ain_contracts::u256_to_h256(new_balance))],
        })
    }
}
