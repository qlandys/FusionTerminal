package main

import (
	"fmt"
	"strings"

	"github.com/shopspring/decimal"
)

var scaleX8Decimal = decimal.RequireFromString("100000000")

func scaleToX8String(s string) (string, error) {
	s = strings.TrimSpace(s)
	if s == "" {
		return "", fmt.Errorf("empty number")
	}
	d, err := decimal.NewFromString(s)
	if err != nil {
		return "", err
	}
	// Truncate to avoid accidentally exceeding size/price.
	scaled := d.Mul(scaleX8Decimal).Truncate(0)
	return scaled.String(), nil
}

