package main

import (
	"errors"
	"fmt"
	"math/big"

	"github.com/consensys/gnark-crypto/ecc/stark-curve/fp"
	"github.com/dontpanicdao/caigo"
	"github.com/dontpanicdao/caigo/types"

	pedersenhash "github.com/consensys/gnark-crypto/ecc/stark-curve/pedersen-hash"
)

var snMessageBigInt = types.UTF8StrToBig("StarkNet Message")

type OnboardingPayload struct {
	Action string
}

func (o *OnboardingPayload) FmtDefinitionEncoding(field string) (fmtEnc []*big.Int) {
	if field == "action" {
		fmtEnc = append(fmtEnc, types.StrToFelt(o.Action).Big())
	}
	return
}

type AuthPayload struct {
	Method     string
	Path       string
	Body       string
	Timestamp  string
	Expiration string
}

func (o *AuthPayload) FmtDefinitionEncoding(field string) (fmtEnc []*big.Int) {
	switch field {
	case "method":
		fmtEnc = append(fmtEnc, types.StrToFelt(o.Method).Big())
	case "path":
		fmtEnc = append(fmtEnc, types.StrToFelt(o.Path).Big())
	case "body":
		// Required as types.StrToFelt("") returns nil (Starknet quirk).
		fmtEnc = append(fmtEnc, big.NewInt(0))
	case "timestamp":
		fmtEnc = append(fmtEnc, types.StrToFelt(o.Timestamp).Big())
	case "expiration":
		if o.Expiration != "" {
			fmtEnc = append(fmtEnc, types.StrToFelt(o.Expiration).Big())
		}
	}
	return fmtEnc
}

type OrderPayload struct {
	Timestamp int64
	Market    string
	Side      string
	OrderType string
	Size      string
	Price     string
}

func (o *OrderPayload) FmtDefinitionEncoding(field string) (fmtEnc []*big.Int) {
	switch field {
	case "timestamp":
		fmtEnc = append(fmtEnc, big.NewInt(o.Timestamp))
	case "market":
		fmtEnc = append(fmtEnc, types.StrToFelt(o.Market).Big())
	case "side":
		side := OrderSide(o.Side)
		if side == OrderSideBuy {
			fmtEnc = append(fmtEnc, types.StrToFelt("1").Big())
		} else {
			fmtEnc = append(fmtEnc, types.StrToFelt("2").Big())
		}
	case "orderType":
		fmtEnc = append(fmtEnc, types.StrToFelt(o.OrderType).Big())
	case "size":
		fmtEnc = append(fmtEnc, types.StrToFelt(o.Size).Big())
	case "price":
		fmtEnc = append(fmtEnc, types.StrToFelt(o.Price).Big())
	}
	return fmtEnc
}

func domainDefinition() *caigo.TypeDef {
	return &caigo.TypeDef{Definitions: []caigo.Definition{
		{Name: "name", Type: "felt"},
		{Name: "chainId", Type: "felt"},
		{Name: "version", Type: "felt"},
	}}
}

func domain(chainId string) *caigo.Domain {
	return &caigo.Domain{
		Name:    "Paradex",
		Version: "1",
		ChainId: chainId,
	}
}

func onboardingPayloadDefinition() *caigo.TypeDef {
	return &caigo.TypeDef{Definitions: []caigo.Definition{
		{Name: "action", Type: "felt"},
	}}
}

func authPayloadDefinition() *caigo.TypeDef {
	return &caigo.TypeDef{Definitions: []caigo.Definition{
		{Name: "method", Type: "felt"},
		{Name: "path", Type: "felt"},
		{Name: "body", Type: "felt"},
		{Name: "timestamp", Type: "felt"},
		{Name: "expiration", Type: "felt"},
	}}
}

func orderPayloadDefinition() *caigo.TypeDef {
	return &caigo.TypeDef{Definitions: []caigo.Definition{
		{Name: "timestamp", Type: "felt"},
		{Name: "market", Type: "felt"},
		{Name: "side", Type: "felt"},
		{Name: "orderType", Type: "felt"},
		{Name: "size", Type: "felt"},
		{Name: "price", Type: "felt"},
	}}
}

func onboardingTypes() map[string]caigo.TypeDef {
	return map[string]caigo.TypeDef{
		"StarkNetDomain": *domainDefinition(),
		"Constant":       *onboardingPayloadDefinition(),
	}
}

func authTypes() map[string]caigo.TypeDef {
	return map[string]caigo.TypeDef{
		"StarkNetDomain": *domainDefinition(),
		"Request":        *authPayloadDefinition(),
	}
}

func orderTypes() map[string]caigo.TypeDef {
	return map[string]caigo.TypeDef{
		"StarkNetDomain": *domainDefinition(),
		"Order":          *orderPayloadDefinition(),
	}
}

func NewVerificationTypedData(vType VerificationType, chainId string) (*caigo.TypedData, error) {
	if vType == VerificationTypeOnboarding {
		return NewTypedData(onboardingTypes(), domain(chainId), "Constant")
	}
	if vType == VerificationTypeAuth {
		return NewTypedData(authTypes(), domain(chainId), "Request")
	}
	if vType == VerificationTypeOrder {
		return NewTypedData(orderTypes(), domain(chainId), "Order")
	}
	return nil, errors.New("invalid validation type")
}

func NewTypedData(typesMap map[string]caigo.TypeDef, domain *caigo.Domain, pType string) (*caigo.TypedData, error) {
	typedData, err := caigo.NewTypedData(typesMap, pType, *domain)
	if err != nil {
		return nil, errors.New("failed to create typed data with caigo")
	}
	return &typedData, nil
}

func PedersenArray(elems []*big.Int) *big.Int {
	fpElements := make([]*fp.Element, len(elems))
	for i, elem := range elems {
		fpElements[i] = new(fp.Element).SetBigInt(elem)
	}
	hash := pedersenhash.PedersenArray(fpElements...)
	return hash.BigInt(new(big.Int))
}

func GnarkGetMessageHash(td *caigo.TypedData, domEnc *big.Int, account *big.Int, msg caigo.TypedMessage, sc caigo.StarkCurve) (hash *big.Int, err error) {
	msgEnc, err := GnarkGetTypedMessageHash(td, td.PrimaryType, msg)
	if err != nil {
		return nil, fmt.Errorf("could not hash message: %w", err)
	}
	elements := []*big.Int{snMessageBigInt, domEnc, account, msgEnc}
	hash = PedersenArray(elements)
	return hash, err
}

func GnarkGetTypedMessageHash(td *caigo.TypedData, inType string, msg caigo.TypedMessage) (hash *big.Int, err error) {
	prim := td.Types[inType]
	elements := make([]*big.Int, 0, len(prim.Definitions)+1)
	elements = append(elements, prim.Encoding)

	for _, def := range prim.Definitions {
		if def.Type == "felt" {
			fmtDefinitions := msg.FmtDefinitionEncoding(def.Name)
			elements = append(elements, fmtDefinitions...)
			continue
		}
		return nil, fmt.Errorf("unsupported field type: %s", def.Type)
	}

	hash = PedersenArray(elements)
	return hash, err
}
