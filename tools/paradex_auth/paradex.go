package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"strconv"
	"time"

	"github.com/dontpanicdao/caigo"
	"github.com/dontpanicdao/caigo/types"
)

func GetParadexConfig(apiBase string) (SystemConfigResponse, error) {
	url := fmt.Sprintf("%s/system/config", apiBase)
	res, err := http.Get(url)
	if err != nil {
		return SystemConfigResponse{}, err
	}
	defer res.Body.Close()
	body, err := io.ReadAll(res.Body)
	if err != nil {
		return SystemConfigResponse{}, err
	}
	if res.StatusCode < 200 || res.StatusCode >= 300 {
		return SystemConfigResponse{}, fmt.Errorf("http %d: %s", res.StatusCode, string(body))
	}
	var cfg SystemConfigResponse
	if err := json.Unmarshal(body, &cfg); err != nil {
		return SystemConfigResponse{}, err
	}
	return cfg, nil
}

func PerformOnboarding(apiBase string,
	cfg SystemConfigResponse,
	ethAddress string,
	dexPrivateKey string,
	dexPublicKey string,
	dexAccountAddress string,
) error {
	dexAccountAddressBN := types.HexToBN(dexAccountAddress)
	sc := caigo.StarkCurve{}
	message := &OnboardingPayload{Action: "Onboarding"}
	typedData, err := NewVerificationTypedData(VerificationTypeOnboarding, cfg.ChainId)
	if err != nil {
		return err
	}
	domEnc, err := typedData.GetTypedMessageHash("StarkNetDomain", typedData.Domain, sc)
	if err != nil {
		return err
	}
	messageHash, err := GnarkGetMessageHash(typedData, domEnc, dexAccountAddressBN, message, sc)
	if err != nil {
		return err
	}
	r, s, err := GnarkSign(messageHash, dexPrivateKey)
	if err != nil {
		return err
	}

	onboardingUrl := fmt.Sprintf("%s/onboarding", apiBase)
	reqBody := OnboardingReqBody{PublicKey: dexPublicKey}
	bodyBytes, err := json.Marshal(reqBody)
	if err != nil {
		return err
	}
	req, err := http.NewRequest(http.MethodPost, onboardingUrl, bytes.NewReader(bodyBytes))
	if err != nil {
		return err
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Add("PARADEX-ETHEREUM-ACCOUNT", ethAddress)
	req.Header.Add("PARADEX-STARKNET-ACCOUNT", dexAccountAddress)
	req.Header.Add("PARADEX-STARKNET-SIGNATURE", GetSignatureStr(r, s))

	res, err := http.DefaultClient.Do(req)
	if err != nil {
		return err
	}
	defer res.Body.Close()
	raw, _ := io.ReadAll(res.Body)
	if res.StatusCode >= 200 && res.StatusCode < 300 {
		return nil
	}
	// Some backends may return a conflict if already onboarded; treat that as success.
	if res.StatusCode == 409 {
		return nil
	}
	if bytes.Contains(bytes.ToUpper(raw), []byte("ALREADY_ONBOARDED")) {
		return nil
	}
	return fmt.Errorf("http %d: %s", res.StatusCode, string(raw))
}

func GetJwtToken(apiBase string,
	cfg SystemConfigResponse,
	dexAccountAddress string,
	dexPrivateKey string,
	expirySeconds int64,
) (jwt string, expiresAt int64, err error) {
	dexAccountAddressBN := types.HexToBN(dexAccountAddress)

	now := time.Now().Unix()
	timestampStr := strconv.FormatInt(now, 10)
	expiresAt = now + expirySeconds
	expirationStr := strconv.FormatInt(expiresAt, 10)

	sc := caigo.StarkCurve{}
	message := &AuthPayload{
		Method:     "POST",
		Path:       "/v1/auth",
		Body:       "",
		Timestamp:  timestampStr,
		Expiration: expirationStr,
	}
	typedData, err := NewVerificationTypedData(VerificationTypeAuth, cfg.ChainId)
	if err != nil {
		return "", 0, err
	}
	domEnc, err := typedData.GetTypedMessageHash("StarkNetDomain", typedData.Domain, sc)
	if err != nil {
		return "", 0, err
	}
	messageHash, err := GnarkGetMessageHash(typedData, domEnc, dexAccountAddressBN, message, sc)
	if err != nil {
		return "", 0, err
	}
	r, s, err := GnarkSign(messageHash, dexPrivateKey)
	if err != nil {
		return "", 0, err
	}

	authUrl := fmt.Sprintf("%s/auth", apiBase)
	req, err := http.NewRequest(http.MethodPost, authUrl, nil)
	if err != nil {
		return "", 0, err
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Add("PARADEX-STARKNET-ACCOUNT", dexAccountAddress)
	req.Header.Add("PARADEX-STARKNET-SIGNATURE", GetSignatureStr(r, s))
	req.Header.Add("PARADEX-TIMESTAMP", timestampStr)
	req.Header.Add("PARADEX-SIGNATURE-EXPIRATION", expirationStr)

	res, err := http.DefaultClient.Do(req)
	if err != nil {
		return "", 0, err
	}
	defer res.Body.Close()
	raw, err := io.ReadAll(res.Body)
	if err != nil {
		return "", 0, err
	}
	if res.StatusCode < 200 || res.StatusCode >= 300 {
		return "", 0, fmt.Errorf("http %d: %s", res.StatusCode, string(raw))
	}
	var out AuthResBody
	if err := json.Unmarshal(raw, &out); err != nil {
		return "", 0, err
	}
	if out.JwtToken == "" {
		return "", 0, fmt.Errorf("empty jwt token")
	}
	return out.JwtToken, expiresAt, nil
}
