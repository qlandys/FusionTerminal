package main

import (
	"fmt"
	"math/big"

	"github.com/consensys/gnark-crypto/ecc/stark-curve"
	"github.com/consensys/gnark-crypto/ecc/stark-curve/ecdsa"
	"github.com/consensys/gnark-crypto/ecc/stark-curve/fr"
	"github.com/dontpanicdao/caigo"
	"github.com/dontpanicdao/caigo/types"
)

func StarkPublicKeyFromPrivateKey(privKey string) (string, error) {
	privBN := types.HexToBN(privKey)
	if privBN == nil {
		return "", fmt.Errorf("invalid private key")
	}
	pubX, _, err := caigo.Curve.PrivateToPoint(privBN)
	if err != nil {
		return "", err
	}
	return types.BigToHex(pubX), nil
}

func GetEcdsaPrivateKey(pk string) (*ecdsa.PrivateKey, error) {
	privateKey := types.StrToFelt(pk).Big()
	if privateKey == nil {
		return nil, fmt.Errorf("invalid private key")
	}

	_, g := starkcurve.Generators()
	pub := new(ecdsa.PublicKey)
	pub.A.ScalarMultiplication(&g, privateKey)

	ecdsaPrivateKey := new(ecdsa.PrivateKey)
	pkBytes := privateKey.FillBytes(make([]byte, fr.Bytes))
	buf := append(pub.Bytes(), pkBytes...)
	if _, err := ecdsaPrivateKey.SetBytes(buf); err != nil {
		return nil, err
	}
	return ecdsaPrivateKey, nil
}

func GnarkSign(messageHash *big.Int, privateKey string) (r, s *big.Int, err error) {
	ecdsaPrivateKey, err := GetEcdsaPrivateKey(privateKey)
	if err != nil {
		return nil, nil, err
	}
	sigBin, err := ecdsaPrivateKey.Sign(messageHash.Bytes(), nil)
	if err != nil {
		return nil, nil, err
	}
	r = new(big.Int).SetBytes(sigBin[:fr.Bytes])
	s = new(big.Int).SetBytes(sigBin[fr.Bytes:])
	return r, s, nil
}
