#!/usr/bin/env python3
# Copyright (c) 2014-2019 The Bitcoin Core developers
# Copyright (c) DeFi Blockchain Developers
# Distributed under the MIT software license, see the accompanying
# file LICENSE or http://www.opensource.org/licenses/mit-license.php.
"""Test listaccounthistory RPC."""

from test_framework.test_framework import DefiTestFramework

from test_framework.util import (
    assert_equal,
)

class RPClistCustomTxTypes(DefiTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [
            ['-acindex=1', '-txnotokens=0', '-amkheight=50', '-bayfrontheight=50', '-bayfrontgardensheight=50'],
        ]

    def run_test(self):
        self.nodes[0].generate(101)

        # collateral address
        collateral_a = self.nodes[0].getnewaddress("", "legacy")

        # Create token
        self.nodes[0].createtoken({
            "symbol": "GOLD",
            "name": "gold",
            "collateralAddress": collateral_a
        })
        self.nodes[0].generate(1)

        # Get token ID
        list_tokens = self.nodes[0].listtokens()
        for idx, token in list_tokens.items():
            if (token["symbol"] == "GOLD"):
                token_a = idx

        # Mint some tokens
        self.nodes[0].minttokens(["300@" + token_a])
        self.nodes[0].generate(1)

        tx_list = self.nodes[0].listcustomtxtypes()
        print("tx_list", tx_list)

        list_history = self.nodes[0].listaccounthistory("mine", {"txtype": tx_list["MintToken"]})
        assert_equal(len(list_history), 1)
        assert_equal(list_history[0]["type"], "MintToken")

        burn_history = self.nodes[0].listburnhistory({"txtype": tx_list["CreateToken"]})
        assert_equal(len(burn_history), 1)
        assert_equal(burn_history[0]["type"], "CreateToken")

        list_history_count = self.nodes[0].accounthistorycount()
        list_history_count_mint = self.nodes[0].accounthistorycount("mine", {"txtype": tx_list["MintToken"]})
        assert(list_history_count_mint < list_history_count)
        assert_equal(list_history_count_mint, 1)

if __name__ == '__main__':
    RPClistCustomTxTypes().main ()
