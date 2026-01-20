package main

import (
	"bufio"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"os"
	"strings"
	"time"

	"github.com/dontpanicdao/caigo"
	"github.com/dontpanicdao/caigo/types"
)

type output struct {
	JWT       string `json:"jwt"`
	Account   string `json:"account"`
	PublicKey string `json:"publicKey"`
	ExpiresAt int64  `json:"expiresAt"`
}

type orderSigOutput struct {
	Signature          string `json:"signature"`
	SignatureTimestamp int64  `json:"signatureTimestamp"`
}

type errOutput struct {
	Error string `json:"error"`
}

func isSupportedOrderType(typ string) bool {
	switch typ {
	case string(OrderTypeLimit),
		string(OrderTypeMarket),
		string(OrderTypeStopMarket),
		string(OrderTypeStopLossMarket),
		string(OrderTypeTakeProfitMarket):
		return true
	default:
		return false
	}
}

func fatalJSON(msg string) {
	enc := json.NewEncoder(os.Stdout)
	_ = enc.Encode(errOutput{Error: msg})
	os.Exit(1)
}

type signReq struct {
	Id          int64  `json:"id"`
	Account     string `json:"account"`
	Market      string `json:"market"`
	Side        string `json:"side"`
	OrderType   string `json:"orderType"`
	Size        string `json:"size"`
	Price       string `json:"price"`
	TimestampMs int64  `json:"timestampMs"`
}

type signResp struct {
	Id                 int64  `json:"id"`
	Signature          string `json:"signature,omitempty"`
	SignatureTimestamp int64  `json:"signatureTimestamp,omitempty"`
	Error              string `json:"error,omitempty"`
}

func main() {
	mode := flag.String("mode", "auth", "Mode: auth | sign-order | serve-sign")
	apiBase := flag.String("api-base", "https://api.prod.paradex.trade/v1", "Paradex REST base URL (e.g. https://api.prod.paradex.trade/v1)")
	l1Address := flag.String("l1-address", "", "Ethereum account address (0x...) used for onboarding")
	l2PrivateKey := flag.String("l2-private-key", "", "Paradex L2 private key (Stark key, 0x...)")
	l2PrivateKeyStdin := flag.Bool("l2-private-key-stdin", false, "Read L2 private key from stdin (preferred over argv for secrecy)")
	expirySeconds := flag.Int64("expiry-seconds", 300, "Auth signature expiration in seconds")
	doOnboard := flag.Bool("onboard", true, "Attempt onboarding before auth (ignored if l1-address is empty)")

	orderAccount := flag.String("account", "", "Paradex Starknet account address (0x...) for sign-order")
	orderMarket := flag.String("market", "", "Market symbol, e.g. ETH-USD-PERP")
	orderSide := flag.String("side", "", "BUY or SELL")
	orderType := flag.String("order-type", "", "LIMIT | MARKET | STOP_MARKET | STOP_LOSS_MARKET | TAKE_PROFIT_MARKET")
	orderSize := flag.String("size", "", "Order size (base units, decimal)")
	orderPrice := flag.String("price", "", "Order price (decimal); for MARKET use 0 or leave empty")
	orderTsMs := flag.Int64("timestamp-ms", 0, "Signature timestamp (ms). 0 = now")
	flag.Parse()

	privKey := strings.TrimSpace(*l2PrivateKey)
	var stdinReader *bufio.Reader
	if *mode == "serve-sign" && stdinReader == nil {
		stdinReader = bufio.NewReader(os.Stdin)
	}
	if privKey == "" && *l2PrivateKeyStdin {
		if *mode == "serve-sign" {
			if stdinReader == nil {
				stdinReader = bufio.NewReader(os.Stdin)
			}
			line, err := stdinReader.ReadString('\n')
			if err != nil && err != io.EOF {
				fatalJSON(fmt.Sprintf("read stdin: %v", err))
			}
			privKey = strings.TrimSpace(line)
		} else {
			raw, err := io.ReadAll(os.Stdin)
			if err != nil {
				fatalJSON(fmt.Sprintf("read stdin: %v", err))
			}
			privKey = strings.TrimSpace(string(raw))
		}
	}
	if privKey == "" {
		fatalJSON("missing L2 private key (use --l2-private-key-stdin)")
	}
	if *mode != "sign-order" && *mode != "serve-sign" && *expirySeconds < 10 {
		fatalJSON("expiry-seconds too small")
	}

	cfg, err := GetParadexConfig(*apiBase)
	if err != nil {
		fatalJSON(fmt.Sprintf("config: %v", err))
	}

	if *mode == "sign-order" {
		if strings.TrimSpace(*orderAccount) == "" {
			fatalJSON("missing --account")
		}
		market := strings.TrimSpace(*orderMarket)
		side := strings.ToUpper(strings.TrimSpace(*orderSide))
		typ := strings.ToUpper(strings.TrimSpace(*orderType))
		size := strings.TrimSpace(*orderSize)
		price := strings.TrimSpace(*orderPrice)
		if market == "" || side == "" || typ == "" || size == "" {
			fatalJSON("missing order params (need --market --side --order-type --size)")
		}
		if !isSupportedOrderType(typ) {
			fatalJSON("invalid --order-type")
		}
		if side != string(OrderSideBuy) && side != string(OrderSideSell) {
			fatalJSON("invalid --side (BUY or SELL)")
		}
		if strings.Contains(typ, "MARKET") && price == "" {
			price = "0"
		}
		if price == "" {
			fatalJSON("missing --price")
		}
		ts := *orderTsMs
		if ts <= 0 {
			ts = time.Now().UnixMilli()
		}
		sizeScaled, err := scaleToX8String(size)
		if err != nil {
			fatalJSON(fmt.Sprintf("size: %v", err))
		}
		priceScaled, err := scaleToX8String(price)
		if err != nil {
			fatalJSON(fmt.Sprintf("price: %v", err))
		}

		sc := caigo.StarkCurve{}
		td, err := NewVerificationTypedData(VerificationTypeOrder, cfg.ChainId)
		if err != nil {
			fatalJSON(fmt.Sprintf("typeddata: %v", err))
		}
		domEnc, err := td.GetTypedMessageHash("StarkNetDomain", td.Domain, sc)
		if err != nil {
			fatalJSON(fmt.Sprintf("domain hash: %v", err))
		}
		accountBN := types.HexToBN(strings.TrimSpace(*orderAccount))
		msg := &OrderPayload{
			Timestamp: ts,
			Market:    market,
			Side:      side,
			OrderType: typ,
			Size:      sizeScaled,
			Price:     priceScaled,
		}
		messageHash, err := GnarkGetMessageHash(td, domEnc, accountBN, msg, sc)
		if err != nil {
			fatalJSON(fmt.Sprintf("message hash: %v", err))
		}
		r, s, err := GnarkSign(messageHash, privKey)
		if err != nil {
			fatalJSON(fmt.Sprintf("sign: %v", err))
		}
		enc := json.NewEncoder(os.Stdout)
		_ = enc.Encode(orderSigOutput{
			Signature:          GetSignatureStr(r, s),
			SignatureTimestamp: ts,
		})
		return
	}

	if *mode == "serve-sign" {
		sc := caigo.StarkCurve{}
		td, err := NewVerificationTypedData(VerificationTypeOrder, cfg.ChainId)
		if err != nil {
			fatalJSON(fmt.Sprintf("typeddata: %v", err))
		}
		domEnc, err := td.GetTypedMessageHash("StarkNetDomain", td.Domain, sc)
		if err != nil {
			fatalJSON(fmt.Sprintf("domain hash: %v", err))
		}
		enc := json.NewEncoder(os.Stdout)
		enc.SetEscapeHTML(false)

		scanner := bufio.NewScanner(stdinReader)
		// allow long lines if needed
		scanner.Buffer(make([]byte, 0, 64*1024), 1024*1024)
		for scanner.Scan() {
			line := strings.TrimSpace(scanner.Text())
			if line == "" {
				continue
			}
			if line == "exit" || line == "quit" {
				return
			}
			var req signReq
			if err := json.Unmarshal([]byte(line), &req); err != nil {
				_ = enc.Encode(signResp{Id: 0, Error: fmt.Sprintf("bad json: %v", err)})
				continue
			}
			resp := signResp{Id: req.Id}
			account := strings.TrimSpace(req.Account)
			market := strings.TrimSpace(req.Market)
			side := strings.ToUpper(strings.TrimSpace(req.Side))
			typ := strings.ToUpper(strings.TrimSpace(req.OrderType))
			size := strings.TrimSpace(req.Size)
			price := strings.TrimSpace(req.Price)
			if account == "" || market == "" || side == "" || typ == "" || size == "" {
				resp.Error = "missing params"
				_ = enc.Encode(resp)
				continue
			}
			if !isSupportedOrderType(typ) {
				resp.Error = "invalid orderType"
				_ = enc.Encode(resp)
				continue
			}
			if side != string(OrderSideBuy) && side != string(OrderSideSell) {
				resp.Error = "invalid side"
				_ = enc.Encode(resp)
				continue
			}
			if strings.Contains(typ, "MARKET") && price == "" {
				price = "0"
			}
			if price == "" {
				resp.Error = "missing price"
				_ = enc.Encode(resp)
				continue
			}
			ts := req.TimestampMs
			if ts <= 0 {
				ts = time.Now().UnixMilli()
			}
			sizeScaled, err := scaleToX8String(size)
			if err != nil {
				resp.Error = fmt.Sprintf("size: %v", err)
				_ = enc.Encode(resp)
				continue
			}
			priceScaled, err := scaleToX8String(price)
			if err != nil {
				resp.Error = fmt.Sprintf("price: %v", err)
				_ = enc.Encode(resp)
				continue
			}
			accountBN := types.HexToBN(account)
			msg := &OrderPayload{
				Timestamp: ts,
				Market:    market,
				Side:      side,
				OrderType: typ,
				Size:      sizeScaled,
				Price:     priceScaled,
			}
			messageHash, err := GnarkGetMessageHash(td, domEnc, accountBN, msg, sc)
			if err != nil {
				resp.Error = fmt.Sprintf("message hash: %v", err)
				_ = enc.Encode(resp)
				continue
			}
			r, s, err := GnarkSign(messageHash, privKey)
			if err != nil {
				resp.Error = fmt.Sprintf("sign: %v", err)
				_ = enc.Encode(resp)
				continue
			}
			resp.Signature = GetSignatureStr(r, s)
			resp.SignatureTimestamp = ts
			_ = enc.Encode(resp)
		}
		if err := scanner.Err(); err != nil {
			fatalJSON(fmt.Sprintf("stdin scan: %v", err))
		}
		return
	}

	publicKey, err := StarkPublicKeyFromPrivateKey(privKey)
	if err != nil {
		fatalJSON(fmt.Sprintf("public key: %v", err))
	}
	account := ComputeAddress(cfg, publicKey)

	if *doOnboard && *l1Address != "" {
		if err := PerformOnboarding(*apiBase, cfg, *l1Address, privKey, publicKey, account); err != nil {
			fatalJSON(fmt.Sprintf("onboarding: %v", err))
		}
	}

	jwt, expiresAt, err := GetJwtToken(*apiBase, cfg, account, privKey, *expirySeconds)
	if err != nil {
		fatalJSON(fmt.Sprintf("auth: %v", err))
	}

	// Provide a minimum guard to help callers avoid immediately-expired tokens.
	if expiresAt <= time.Now().Unix() {
		fatalJSON("auth: token already expired")
	}

	enc := json.NewEncoder(os.Stdout)
	_ = enc.Encode(output{
		JWT:       jwt,
		Account:   account,
		PublicKey: publicKey,
		ExpiresAt: expiresAt,
	})
}
