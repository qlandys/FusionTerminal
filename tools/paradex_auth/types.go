package main

type BridgedToken struct {
	Name            string `json:"name"`
	Symbol          string `json:"symbol"`
	Decimals        int    `json:"decimals"`
	L1TokenAddress  string `json:"l1_token_address"`
	L1BridgeAddress string `json:"l1_bridge_address"`
	L2TokenAddress  string `json:"l2_token_address"`
	L2BridgeAddress string `json:"l2_bridge_address"`
}

type SystemConfigResponse struct {
	GatewayUrl                string         `json:"starknet_gateway_url"`
	ChainId                   string         `json:"starknet_chain_id"`
	BlockExplorerUrl          string         `json:"block_explorer_url"`
	ParaclearAddress          string         `json:"paraclear_address"`
	ParaclearDecimals         int            `json:"paraclear_decimals"`
	ParaclearAccountProxyHash string         `json:"paraclear_account_proxy_hash"`
	ParaclearAccountHash      string         `json:"paraclear_account_hash"`
	BridgedTokens             []BridgedToken `json:"bridged_tokens"`
	L1CoreContractAddress     string         `json:"l1_core_contract_address"`
	L1OperatorAddress         string         `json:"l1_operator_address"`
	L1ChainId                 string         `json:"l1_chain_id"`
}

type OnboardingReqBody struct {
	PublicKey string `json:"public_key"`
}

type AuthResBody struct {
	JwtToken string `json:"jwt_token"`
}

type VerificationType string

var (
	VerificationTypeOnboarding VerificationType = "Onboarding"
	VerificationTypeAuth       VerificationType = "Auth"
	VerificationTypeOrder      VerificationType = "Order"
)

type OrderSide string
type OrderType string

const (
	OrderSideBuy  OrderSide = "BUY"
	OrderSideSell OrderSide = "SELL"
)

const (
	OrderTypeMarket OrderType = "MARKET"
	OrderTypeLimit  OrderType = "LIMIT"
	// Stop/trigger orders (execute as market when trigger fires).
	OrderTypeStopMarket       OrderType = "STOP_MARKET"
	OrderTypeStopLossMarket   OrderType = "STOP_LOSS_MARKET"
	OrderTypeTakeProfitMarket OrderType = "TAKE_PROFIT_MARKET"
)
