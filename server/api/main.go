package main

import (
	"crypto/rand"
	"crypto/sha256"
	"crypto/tls"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"math/big"
	"net"
	"net/http"
	"net/smtp"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"
)

type Config struct {
	Addr         string
	DBPath       string
	JWTSecret    string
	JWTIssuer    string
	TokenTTL     time.Duration
	CodeTTL      time.Duration
	SMTPHost     string
	SMTPPort     int
	SMTPUser     string
	SMTPPass     string
	SMTPFrom     string
	SMTPFromName string
	SMTPDisable  bool
}

type Server struct {
	cfg Config
	db  *Store
}

type User struct {
	ID       int64
	Email    string
	Login    string
	PassHash string
	Role     string
	Verified bool
	Created  int64
}

type PendingReg struct {
	Email    string `json:"email"`
	Login    string `json:"login"`
	PassHash string `json:"pass_hash"`
	Created  int64  `json:"created_at"`
}

type EmailCode struct {
	ID        int64
	Email     string
	Code      string
	Purpose   string
	ExpiresAt int64
	Used      bool
	Created   int64
}

type storeData struct {
	NextUserID int64       `json:"next_user_id"`
	NextCodeID int64       `json:"next_code_id"`
	Users      []User      `json:"users"`
	Codes      []EmailCode `json:"codes"`
	Pending    []PendingReg `json:"pending"`
}

type Store struct {
	mu   sync.Mutex
	path string
	data storeData
}

type okResp struct {
	OK bool `json:"ok"`
}

type errorResp struct {
	Error string `json:"error"`
}

type loginReq struct {
	EmailOrLogin string `json:"email_or_login"`
	Password     string `json:"password"`
}

type registerReq struct {
	Email        string `json:"email"`
	Login        string `json:"login"`
	Password     string `json:"password"`
	EmailOrLogin string `json:"email_or_login"`
}

type sendCodeReq struct {
	EmailOrLogin string `json:"email_or_login"`
	Channel      string `json:"channel"`
}

type verifyCodeReq struct {
	EmailOrLogin string `json:"email_or_login"`
	Email        string `json:"email"`
	Login        string `json:"login"`
	Code         string `json:"code"`
	Flow         string `json:"flow"`
}

type authResp struct {
	Token string `json:"token"`
	Role  string `json:"role"`
	User  string `json:"user"`
}

type authClaims struct {
	Role  string `json:"role"`
	User  string `json:"user"`
	Email string `json:"email"`
}

func main() {
	cfg := loadConfig()
	store, err := loadStore(cfg.DBPath)
	if err != nil {
		log.Fatalf("store open: %v", err)
	}

	srv := &Server{cfg: cfg, db: store}
	mux := http.NewServeMux()
	mux.HandleFunc("/health", srv.handleHealth)
	mux.HandleFunc("/auth/send_code", srv.handleSendCode)
	mux.HandleFunc("/auth/register", srv.handleRegister)
	mux.HandleFunc("/auth/verify_code", srv.handleVerifyCode)
	mux.HandleFunc("/auth/login", srv.handleLogin)

	log.Printf("Fusion API listening on %s", cfg.Addr)
	if err := http.ListenAndServe(cfg.Addr, withJSON(mux)); err != nil {
		log.Fatalf("listen: %v", err)
	}
}

func loadConfig() Config {
	cfg := Config{
		Addr:         getenv("ADDR", ":8686"),
		DBPath:       getenv("DB_PATH", "./fusion.db"),
		JWTSecret:    os.Getenv("JWT_SECRET"),
		JWTIssuer:    getenv("JWT_ISSUER", "fusionterminal"),
		TokenTTL:     parseDuration(getenv("TOKEN_TTL", "168h")),
		CodeTTL:      parseDuration(getenv("CODE_TTL", "10m")),
		SMTPHost:     os.Getenv("SMTP_HOST"),
		SMTPUser:     os.Getenv("SMTP_USER"),
		SMTPPass:     os.Getenv("SMTP_PASS"),
		SMTPFrom:     os.Getenv("SMTP_FROM"),
		SMTPFromName: os.Getenv("SMTP_FROM_NAME"),
	}
	cfg.SMTPDisable = getenv("SMTP_DISABLED", "") == "1"
	portStr := getenv("SMTP_PORT", "587")
	port, err := strconv.Atoi(portStr)
	if err != nil {
		port = 587
	}
	cfg.SMTPPort = port
	if cfg.JWTSecret == "" {
		log.Fatal("JWT_SECRET is required")
	}
	return cfg
}

func withJSON(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		next.ServeHTTP(w, r)
	})
}

func (s *Server) handleHealth(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, okResp{OK: true})
}

func (s *Server) handleSendCode(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeJSON(w, http.StatusMethodNotAllowed, errorResp{Error: "method not allowed"})
		return
	}
	var req sendCodeReq
	if err := decodeJSON(r, &req); err != nil {
		writeJSON(w, http.StatusBadRequest, errorResp{Error: "invalid json"})
		return
	}
	if req.Channel != "" && req.Channel != "email" {
		writeJSON(w, http.StatusBadRequest, errorResp{Error: "unsupported channel"})
		return
	}
	emailOrLogin := strings.TrimSpace(req.EmailOrLogin)
	if emailOrLogin == "" {
		writeJSON(w, http.StatusBadRequest, errorResp{Error: "email required"})
		return
	}
	user, err := s.db.getUserByLoginOrEmail(emailOrLogin)
	pending, _ := s.db.getPendingByEmailOrLogin(emailOrLogin)
	if err != nil && pending == nil {
		writeJSON(w, http.StatusBadRequest, errorResp{Error: "user not found"})
		return
	}
	purpose := "login"
	sendTo := ""
	if pending != nil {
		purpose = "register"
		sendTo = pending.Email
	} else if user != nil {
		if !user.Verified {
			purpose = "register"
		}
		sendTo = user.Email
	}
	if sendTo == "" {
		writeJSON(w, http.StatusBadRequest, errorResp{Error: "user not found"})
		return
	}
	if err := s.issueEmailCode(sendTo, purpose); err != nil {
		writeJSON(w, http.StatusInternalServerError, errorResp{Error: "send failed"})
		return
	}
	writeJSON(w, http.StatusOK, okResp{OK: true})
}

func (s *Server) handleRegister(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeJSON(w, http.StatusMethodNotAllowed, errorResp{Error: "method not allowed"})
		return
	}
	var req registerReq
	if err := decodeJSON(r, &req); err != nil {
		writeJSON(w, http.StatusBadRequest, errorResp{Error: "invalid json"})
		return
	}
	email := strings.TrimSpace(req.Email)
	if email == "" {
		email = strings.TrimSpace(req.EmailOrLogin)
	}
	login := strings.TrimSpace(req.Login)
	pass := req.Password
	if err := validateRegister(email, login, pass); err != nil {
		writeJSON(w, http.StatusBadRequest, errorResp{Error: err.Error()})
		return
	}
	if user, exists := s.db.userExists(email, login); exists {
		if user != nil && user.Verified {
			if strings.EqualFold(user.Email, email) {
				writeJSON(w, http.StatusConflict, errorResp{Error: "email_taken"})
				return
			}
			if strings.EqualFold(user.Login, login) {
				writeJSON(w, http.StatusConflict, errorResp{Error: "login_taken"})
				return
			}
			writeJSON(w, http.StatusConflict, errorResp{Error: "user_exists"})
			return
		}
	}
	if pending, ok := s.db.getPendingByEmailOrLogin(email); ok && pending != nil {
		if err := s.issueEmailCode(pending.Email, "register"); err != nil {
			writeJSON(w, http.StatusInternalServerError, errorResp{Error: "send failed"})
			return
		}
		writeJSON(w, http.StatusOK, okResp{OK: true})
		return
	}
	if pending, ok := s.db.getPendingByEmailOrLogin(login); ok && pending != nil {
		if err := s.issueEmailCode(pending.Email, "register"); err != nil {
			writeJSON(w, http.StatusInternalServerError, errorResp{Error: "send failed"})
			return
		}
		writeJSON(w, http.StatusOK, okResp{OK: true})
		return
	}
	hash, err := hashPassword(pass)
	if err != nil {
		writeJSON(w, http.StatusInternalServerError, errorResp{Error: "hash failed"})
		return
	}
	if err := s.db.addPending(PendingReg{
		Email:    email,
		Login:    login,
		PassHash: hash,
		Created:  time.Now().Unix(),
	}); err != nil {
		writeJSON(w, http.StatusInternalServerError, errorResp{Error: "register failed"})
		return
	}
	if err := s.issueEmailCode(email, "register"); err != nil {
		writeJSON(w, http.StatusInternalServerError, errorResp{Error: "send failed"})
		return
	}
	writeJSON(w, http.StatusOK, okResp{OK: true})
}

func (s *Server) handleVerifyCode(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeJSON(w, http.StatusMethodNotAllowed, errorResp{Error: "method not allowed"})
		return
	}
	var req verifyCodeReq
	if err := decodeJSON(r, &req); err != nil {
		writeJSON(w, http.StatusBadRequest, errorResp{Error: "invalid json"})
		return
	}
	email := strings.TrimSpace(req.Email)
	if email == "" {
		email, _ = s.resolveEmail(req.EmailOrLogin, req.Login)
	}
	code := strings.TrimSpace(req.Code)
	if email == "" || code == "" {
		writeJSON(w, http.StatusBadRequest, errorResp{Error: "missing email or code"})
		return
	}
	flow := strings.TrimSpace(req.Flow)
	if flow == "" {
		flow = "login"
	}
	if err := s.consumeCode(email, code, flow); err != nil {
		altFlow := "login"
		if flow == "login" {
			altFlow = "register"
		}
		if err2 := s.consumeCode(email, code, altFlow); err2 != nil {
			writeJSON(w, http.StatusUnauthorized, errorResp{Error: "invalid code"})
			return
		}
		flow = altFlow
	}
	var user *User
	var err error
	if flow == "register" {
		pending, ok := s.db.getPendingByEmailOrLogin(email)
		if !ok || pending == nil {
			writeJSON(w, http.StatusUnauthorized, errorResp{Error: "registration_not_found"})
			return
		}
		if err := s.db.insertUserFromPending(pending); err != nil {
			writeJSON(w, http.StatusInternalServerError, errorResp{Error: "verify failed"})
			return
		}
		user, _ = s.db.getUserByEmail(pending.Email)
	} else {
		user, err = s.db.getUserByEmail(email)
		if err != nil {
			writeJSON(w, http.StatusUnauthorized, errorResp{Error: "user not found"})
			return
		}
	}
	token, err := s.issueToken(user)
	if err != nil {
		writeJSON(w, http.StatusInternalServerError, errorResp{Error: "token failed"})
		return
	}
	writeJSON(w, http.StatusOK, authResp{
		Token: token,
		Role:  user.Role,
		User:  user.Login,
	})
}

func (s *Server) handleLogin(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeJSON(w, http.StatusMethodNotAllowed, errorResp{Error: "method not allowed"})
		return
	}
	var req loginReq
	if err := decodeJSON(r, &req); err != nil {
		writeJSON(w, http.StatusBadRequest, errorResp{Error: "invalid json"})
		return
	}
	emailOrLogin := strings.TrimSpace(req.EmailOrLogin)
	if emailOrLogin == "" || req.Password == "" {
		writeJSON(w, http.StatusBadRequest, errorResp{Error: "missing credentials"})
		return
	}
	user, err := s.db.getUserByLoginOrEmail(emailOrLogin)
	if err != nil {
		writeJSON(w, http.StatusUnauthorized, errorResp{Error: "invalid credentials"})
		return
	}
	if !user.Verified {
		writeJSON(w, http.StatusUnauthorized, errorResp{Error: "email not verified"})
		return
	}
	if !checkPassword(user.PassHash, req.Password) {
		writeJSON(w, http.StatusUnauthorized, errorResp{Error: "invalid credentials"})
		return
	}
	token, err := s.issueToken(user)
	if err != nil {
		writeJSON(w, http.StatusInternalServerError, errorResp{Error: "token failed"})
		return
	}
	writeJSON(w, http.StatusOK, authResp{
		Token: token,
		Role:  user.Role,
		User:  user.Login,
	})
}

func (s *Server) resolveEmail(emailOrLogin string, login string) (string, error) {
	val := strings.TrimSpace(emailOrLogin)
	if val == "" {
		val = strings.TrimSpace(login)
	}
	if strings.Contains(val, "@") {
		return val, nil
	}
	if val == "" {
		return "", errors.New("email required")
	}
	user, err := s.db.getUserByLoginOrEmail(val)
	if err != nil {
		return "", errors.New("user not found")
	}
	return user.Email, nil
}

func validateRegister(email, login, password string) error {
	if email == "" || !strings.Contains(email, "@") {
		return errors.New("invalid email")
	}
	if login == "" {
		return errors.New("login required")
	}
	if len(password) < 6 {
		return errors.New("password too short")
	}
	return nil
}

func (s *Server) issueEmailCode(email, purpose string) error {
	code := randomCode(6)
	expires := time.Now().Add(s.cfg.CodeTTL).Unix()
	if err := s.db.addCode(EmailCode{
		Email:     email,
		Code:      code,
		Purpose:   purpose,
		ExpiresAt: expires,
		Used:      false,
		Created:   time.Now().Unix(),
	}); err != nil {
		return err
	}
	subject := "FusionTerminal verification code"
	textBody := fmt.Sprintf("Your code: %s\nThis code expires in %d minutes.", code, int(s.cfg.CodeTTL.Minutes()))
	htmlBody := buildHTMLMessage(code, int(s.cfg.CodeTTL.Minutes()))
	log.Printf("email code for %s (%s): %s", email, purpose, code)
	if err := s.sendEmail(email, subject, textBody, htmlBody); err != nil {
		log.Printf("email send failed to %s: %v", email, err)
		return err
	}
	log.Printf("email sent to %s for %s", email, purpose)
	return nil
}

func (s *Server) consumeCode(email, code, purpose string) error {
	return s.db.consumeCode(email, code, purpose)
}

func (s *Server) issueToken(user *User) (string, error) {
	token, err := randomToken(32)
	if err != nil {
		return "", err
	}
	_ = authClaims{
		Role:  user.Role,
		User:  user.Login,
		Email: user.Email,
	}
	return token, nil
}

func (s *Server) sendEmail(to, subject, textBody, htmlBody string) error {
	if s.cfg.SMTPDisable {
		log.Printf("SMTP disabled; code for %s: %s", to, textBody)
		return nil
	}
	if s.cfg.SMTPHost == "" || s.cfg.SMTPUser == "" || s.cfg.SMTPPass == "" {
		return errors.New("smtp not configured")
	}
	fromAddr := s.cfg.SMTPFrom
	if fromAddr == "" {
		fromAddr = s.cfg.SMTPUser
	}
	fromHeader := fromAddr
	if s.cfg.SMTPFromName != "" {
		fromHeader = fmt.Sprintf("%s <%s>", s.cfg.SMTPFromName, fromAddr)
	}
	msg := buildMessage(fromHeader, fromAddr, to, subject, textBody, htmlBody)
	addr := net.JoinHostPort(s.cfg.SMTPHost, strconv.Itoa(s.cfg.SMTPPort))
	auth := smtp.PlainAuth("", s.cfg.SMTPUser, s.cfg.SMTPPass, s.cfg.SMTPHost)

	if s.cfg.SMTPPort == 465 {
		return sendMailTLS(addr, auth, fromAddr, []string{to}, msg)
	}
	c, err := smtp.Dial(addr)
	if err != nil {
		return err
	}
	defer c.Close()
	if ok, _ := c.Extension("STARTTLS"); ok {
		tlsConfig := &tls.Config{ServerName: s.cfg.SMTPHost}
		if err := c.StartTLS(tlsConfig); err != nil {
			return err
		}
	}
	if err := c.Auth(auth); err != nil {
		return err
	}
	if err := c.Mail(fromAddr); err != nil {
		return err
	}
	if err := c.Rcpt(to); err != nil {
		return err
	}
	wc, err := c.Data()
	if err != nil {
		return err
	}
	if _, err := wc.Write([]byte(msg)); err != nil {
		return err
	}
	if err := wc.Close(); err != nil {
		return err
	}
	return c.Quit()
}

func sendMailTLS(addr string, auth smtp.Auth, from string, to []string, msg string) error {
	conn, err := tls.Dial("tcp", addr, &tls.Config{ServerName: strings.Split(addr, ":")[0]})
	if err != nil {
		return err
	}
	defer conn.Close()
	c, err := smtp.NewClient(conn, strings.Split(addr, ":")[0])
	if err != nil {
		return err
	}
	defer c.Close()
	if err := c.Auth(auth); err != nil {
		return err
	}
	if err := c.Mail(from); err != nil {
		return err
	}
	for _, addr := range to {
		if err := c.Rcpt(addr); err != nil {
			return err
		}
	}
	wc, err := c.Data()
	if err != nil {
		return err
	}
	if _, err := wc.Write([]byte(msg)); err != nil {
		return err
	}
	if err := wc.Close(); err != nil {
		return err
	}
	return c.Quit()
}

func buildMessage(fromHeader, fromAddr, to, subject, textBody, htmlBody string) string {
	boundary := fmt.Sprintf("fusion-%d", time.Now().UnixNano())
	var sb strings.Builder
	sb.WriteString("From: " + fromHeader + "\r\n")
	sb.WriteString("Sender: " + fromAddr + "\r\n")
	sb.WriteString("Reply-To: " + fromAddr + "\r\n")
	sb.WriteString("To: " + to + "\r\n")
	sb.WriteString("Subject: " + subject + "\r\n")
	sb.WriteString("MIME-Version: 1.0\r\n")
	sb.WriteString("Content-Type: multipart/alternative; boundary=" + boundary + "\r\n")
	sb.WriteString("\r\n")
	sb.WriteString("--" + boundary + "\r\n")
	sb.WriteString("Content-Type: text/plain; charset=utf-8\r\n")
	sb.WriteString("Content-Transfer-Encoding: 7bit\r\n")
	sb.WriteString("\r\n")
	sb.WriteString(textBody + "\r\n")
	sb.WriteString("--" + boundary + "\r\n")
	sb.WriteString("Content-Type: text/html; charset=utf-8\r\n")
	sb.WriteString("Content-Transfer-Encoding: 7bit\r\n")
	sb.WriteString("\r\n")
	sb.WriteString(htmlBody + "\r\n")
	sb.WriteString("--" + boundary + "--\r\n")
	return sb.String()
}

func buildHTMLMessage(code string, ttlMinutes int) string {
	return fmt.Sprintf(`<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
</head>
<body style="margin:0;padding:0;background:#ffffff;color:#0f1114;font-family:Verdana, Arial, sans-serif;">
  <table role="presentation" width="100%%" cellspacing="0" cellpadding="0" border="0" style="background:#ffffff;margin:0;padding:0;font-family:Verdana, Arial, sans-serif;">
    <tr>
      <td align="center" style="padding:28px 16px;">
        <table role="presentation" width="660" cellspacing="0" cellpadding="0" border="0" style="width:660px;max-width:660px;background:#14181d;border:1px solid #242a31;border-radius:0;">
          <tr>
            <td align="center" style="padding:56px 36px;min-height:820px;">
              <div style="font-size:24px;color:#c0c7d0;letter-spacing:0.2px;margin-bottom:20px;">Your verification code</div>
              <div style="height:1px;background:#222830;margin:18px 0 26px;"></div>
              <div style="font-size:50px;letter-spacing:10px;font-weight:700;color:#e6e6e6;">%s</div>
              <div style="height:1px;background:#222830;margin:30px 0 18px;"></div>
              <div style="font-size:18px;color:#c0c7d0;">Code is valid for %d minutes.</div>
              <div style="font-size:18px;color:#9aa3ad;margin-top:14px;">If this wasn’t you, just ignore this email.</div>
              <div style="display:none;color:#ffffff;font-size:1px;line-height:1px;">id:%s-%d</div>
            </td>
          </tr>
        </table>
      </td>
    </tr>
  </table>
</body>
</html>`, code, ttlMinutes, code, time.Now().Unix())
}

func randomCode(length int) string {
	if length <= 0 {
		length = 6
	}
	max := 10
	b := make([]byte, length)
	for i := range b {
		n, err := rand.Int(rand.Reader, big.NewInt(int64(max)))
		if err != nil {
			b[i] = byte('0' + (i % 10))
			continue
		}
		b[i] = byte('0' + n.Int64())
	}
	return string(b)
}

func randomToken(size int) (string, error) {
	if size <= 0 {
		size = 32
	}
	buf := make([]byte, size)
	if _, err := rand.Read(buf); err != nil {
		return "", err
	}
	return hex.EncodeToString(buf), nil
}

func hashPassword(pass string) (string, error) {
	salt := make([]byte, 16)
	if _, err := rand.Read(salt); err != nil {
		return "", err
	}
	sum := sha256.Sum256(append(salt, []byte(pass)...))
	return hex.EncodeToString(salt) + ":" + hex.EncodeToString(sum[:]), nil
}

func checkPassword(stored, pass string) bool {
	parts := strings.SplitN(stored, ":", 2)
	if len(parts) != 2 {
		return false
	}
	salt, err := hex.DecodeString(parts[0])
	if err != nil {
		return false
	}
	sum := sha256.Sum256(append(salt, []byte(pass)...))
	return hex.EncodeToString(sum[:]) == parts[1]
}

func decodeJSON(r *http.Request, out any) error {
	defer r.Body.Close()
	dec := json.NewDecoder(r.Body)
	dec.DisallowUnknownFields()
	return dec.Decode(out)
}

func writeJSON(w http.ResponseWriter, code int, v any) {
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(v)
}

func parseDuration(val string) time.Duration {
	dur, err := time.ParseDuration(val)
	if err != nil {
		return 0
	}
	return dur
}

func getenv(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}

func loadStore(path string) (*Store, error) {
	store := &Store{
		path: path,
		data: storeData{},
	}
	if path == "" {
		return store, nil
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return nil, err
	}
	raw, err := os.ReadFile(path)
	if err != nil {
		if os.IsNotExist(err) {
			return store, nil
		}
		return nil, err
	}
	if len(raw) == 0 {
		return store, nil
	}
	if err := json.Unmarshal(raw, &store.data); err != nil {
		return nil, err
	}
	store.ensureIDs()
	return store, nil
}

func (s *Store) ensureIDs() {
	var maxUser, maxCode int64
	for _, u := range s.data.Users {
		if u.ID > maxUser {
			maxUser = u.ID
		}
	}
	for _, c := range s.data.Codes {
		if c.ID > maxCode {
			maxCode = c.ID
		}
	}
	if s.data.NextUserID <= maxUser {
		s.data.NextUserID = maxUser + 1
	}
	if s.data.NextCodeID <= maxCode {
		s.data.NextCodeID = maxCode + 1
	}
	if s.data.NextUserID == 0 {
		s.data.NextUserID = 1
	}
	if s.data.NextCodeID == 0 {
		s.data.NextCodeID = 1
	}
}

func (s *Store) saveLocked() error {
	raw, err := json.Marshal(s.data)
	if err != nil {
		return err
	}
	tmp := s.path + ".tmp"
	if err := os.WriteFile(tmp, raw, 0o600); err != nil {
		return err
	}
	return os.Rename(tmp, s.path)
}

func (s *Store) userExists(email, login string) (*User, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, u := range s.data.Users {
		if strings.EqualFold(u.Email, email) || strings.EqualFold(u.Login, login) {
			cp := u
			return &cp, true
		}
	}
	return nil, false
}

func (s *Store) insertUser(email, login, passHash string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, u := range s.data.Users {
		if strings.EqualFold(u.Email, email) || strings.EqualFold(u.Login, login) {
			return errors.New("user exists")
		}
	}
	user := User{
		ID:       s.data.NextUserID,
		Email:    email,
		Login:    login,
		PassHash: passHash,
		Role:     "user",
		Verified: false,
		Created:  time.Now().Unix(),
	}
	s.data.NextUserID++
	s.data.Users = append(s.data.Users, user)
	return s.saveLocked()
}

func (s *Store) getUserByEmail(email string) (*User, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, u := range s.data.Users {
		if strings.EqualFold(u.Email, email) {
			cp := u
			return &cp, nil
		}
	}
	return nil, errors.New("user not found")
}

func (s *Store) getUserByLoginOrEmail(val string) (*User, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, u := range s.data.Users {
		if strings.EqualFold(u.Email, val) || strings.EqualFold(u.Login, val) {
			cp := u
			return &cp, nil
		}
	}
	return nil, errors.New("user not found")
}

func (s *Store) addCode(code EmailCode) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	code.ID = s.data.NextCodeID
	s.data.NextCodeID++
	s.data.Codes = append(s.data.Codes, code)
	return s.saveLocked()
}

func (s *Store) addPending(p PendingReg) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, u := range s.data.Users {
		if strings.EqualFold(u.Email, p.Email) || strings.EqualFold(u.Login, p.Login) {
			return errors.New("user exists")
		}
	}
	for _, pr := range s.data.Pending {
		if strings.EqualFold(pr.Email, p.Email) || strings.EqualFold(pr.Login, p.Login) {
			return nil
		}
	}
	s.data.Pending = append(s.data.Pending, p)
	return s.saveLocked()
}

func (s *Store) getPendingByEmailOrLogin(val string) (*PendingReg, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, p := range s.data.Pending {
		if strings.EqualFold(p.Email, val) || strings.EqualFold(p.Login, val) {
			cp := p
			return &cp, true
		}
	}
	return nil, false
}

func (s *Store) insertUserFromPending(p *PendingReg) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, u := range s.data.Users {
		if strings.EqualFold(u.Email, p.Email) || strings.EqualFold(u.Login, p.Login) {
			return errors.New("user exists")
		}
	}
	user := User{
		ID:       s.data.NextUserID,
		Email:    p.Email,
		Login:    p.Login,
		PassHash: p.PassHash,
		Role:     "user",
		Verified: true,
		Created:  time.Now().Unix(),
	}
	s.data.NextUserID++
	s.data.Users = append(s.data.Users, user)
	next := s.data.Pending[:0]
	for _, pr := range s.data.Pending {
		if strings.EqualFold(pr.Email, p.Email) || strings.EqualFold(pr.Login, p.Login) {
			continue
		}
		next = append(next, pr)
	}
	s.data.Pending = next
	return s.saveLocked()
}

func (s *Store) consumeCode(email, code, purpose string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	now := time.Now().Unix()
	for i := len(s.data.Codes) - 1; i >= 0; i-- {
		c := s.data.Codes[i]
		if strings.EqualFold(c.Email, email) && c.Code == code && c.Purpose == purpose {
			if c.Used {
				return errors.New("code used")
			}
			if now > c.ExpiresAt {
				return errors.New("code expired")
			}
			s.data.Codes[i].Used = true
			return s.saveLocked()
		}
	}
	return errors.New("code not found")
}
