package main

import (
	"encoding/json"
	"math/big"

	"github.com/dontpanicdao/caigo"
	"github.com/dontpanicdao/caigo/types"
)

func GetSignatureStr(r, s *big.Int) string {
	signature := []string{r.String(), s.String()}
	signatureByte, _ := json.Marshal(signature)
	return string(signatureByte)
}

func ComputeAddress(config SystemConfigResponse, publicKey string) string {
	publicKeyBN := types.HexToBN(publicKey)

	paraclearAccountHashBN := types.HexToBN(config.ParaclearAccountHash)
	paraclearAccountProxyHashBN := types.HexToBN(config.ParaclearAccountProxyHash)

	zero := big.NewInt(0)
	initializeBN := types.GetSelectorFromName("initialize")

	contractAddressPrefix := types.StrToFelt("STARKNET_CONTRACT_ADDRESS").Big()

	constructorCalldata := []*big.Int{
		paraclearAccountHashBN,
		initializeBN,
		big.NewInt(2),
		publicKeyBN,
		zero,
	}
	constructorCalldataHash, _ := caigo.Curve.ComputeHashOnElements(constructorCalldata)

	address := []*big.Int{
		contractAddressPrefix,
		zero,        // deployer address
		publicKeyBN, // salt
		paraclearAccountProxyHashBN,
		constructorCalldataHash,
	}
	addressHash, _ := caigo.Curve.ComputeHashOnElements(address)
	return types.BigToHex(addressHash)
}

