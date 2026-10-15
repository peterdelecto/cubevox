// Command cubevox-app serves the layout page on 127.0.0.1 and relays it to the box over USB.
package main

import (
	"context"
	"embed"
	"errors"
	"flag"
	"fmt"
	"io/fs"
	"log"
	"net"
	"net/http"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"time"

	"cubevox.app/fixtures"
	"cubevox.app/internal/box"
	"cubevox.app/internal/fake"
	"cubevox.app/internal/life"
	"cubevox.app/internal/server"
	"cubevox.app/internal/update"
)

// version is set with -ldflags "-X main.version=vX.YY.ZZ".
var version = "dev"

//go:embed web
var webFiles embed.FS

func main() {
	if err := run(); err != nil {
		log.Fatalf("cubevox: %v", err)
	}
}

func run() error {
	useFake := flag.Bool("fake", false, "use the fake box")
	port := flag.Int("port", 0, "port on 127.0.0.1 (0 picks a free one)")
	noOpen := flag.Bool("no-open", false, "do not open the browser")
	flag.Parse()

	portFile, err := portFilePath()
	if err != nil {
		return err
	}
	if url, ok := liveInstance(portFile); ok {
		log.Printf("already running at %s", url)
		return openBrowser(url, *noOpen)
	}
	return serve(*useFake, *port, *noOpen, portFile)
}

func serve(useFake bool, port int, noOpen bool, portFile string) error {
	fixture, err := fixtures.Hello()
	if err != nil {
		return err
	}
	web, err := fs.Sub(webFiles, "web")
	if err != nil {
		return fmt.Errorf("embedded page: %w", err)
	}
	idle := life.NewIdle(nil, life.IdleLimit)
	var b box.Box
	var onOpen func()
	var onClose func()
	if useFake {
		b = fake.New(fixture)
	} else {
		link := box.NewLink()
		b, onOpen, onClose = link, link.Start, link.Stop
	}
	ln, err := net.Listen("tcp", net.JoinHostPort("127.0.0.1", strconv.Itoa(port)))
	if err != nil {
		return fmt.Errorf("listen: %w", err)
	}
	actual := ln.Addr().(*net.TCPAddr).Port
	srv := server.New(server.Config{
		Box: b, Fixture: fixture, App: version, Web: web, Idle: idle, OnPageOpen: onOpen,
		CheckUpdate: update.Check,
		RunUpdate:   updateRunner(b, version, actual),
	})
	if err := writePortFile(portFile, actual); err != nil {
		return err
	}
	defer removePortFile(portFile)
	if onClose != nil {
		defer onClose()
	}

	url := fmt.Sprintf("http://127.0.0.1:%d", actual)
	log.Printf("cubevox %s serving %s", version, url)
	httpSrv := &http.Server{Handler: srv.Handler(), ReadHeaderTimeout: 5 * time.Second}
	errc := make(chan error, 1)
	go func() { errc <- httpSrv.Serve(ln) }()
	if err := openBrowser(url, noOpen); err != nil {
		log.Printf("open browser: %v", err)
	}
	return wait(httpSrv, idle, errc)
}

// updateRunner builds the real update for one run. The relaunch step ends this process with
// SIGTERM so wait() shuts the server down and removes the port file.
func updateRunner(b box.Box, app string, port int) func(context.Context, update.Info, func(string)) error {
	quit := func() { _ = syscall.Kill(os.Getpid(), syscall.SIGTERM) }
	return func(ctx context.Context, info update.Info, step func(string)) error {
		deps, err := update.SystemDeps(info, app, b, port, quit)
		if err != nil {
			return err
		}
		deps.Step = step
		return update.Run(ctx, deps)
	}
}

// wait blocks until the idle limit, a signal, or a server failure, then shuts the server down.
func wait(httpSrv *http.Server, idle *life.Idle, errc <-chan error) error {
	sig := make(chan os.Signal, 1)
	signal.Notify(sig, os.Interrupt, syscall.SIGTERM)
	tick := time.NewTicker(time.Second)
	defer tick.Stop()
	for {
		select {
		case err := <-errc:
			return fmt.Errorf("serve: %w", err)
		case <-sig:
			return shutdown(httpSrv)
		case <-tick.C:
			if idle.Expired() {
				log.Printf("idle for %v, exiting", life.IdleLimit)
				return shutdown(httpSrv)
			}
		}
	}
}

func shutdown(srv *http.Server) error {
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	if err := srv.Shutdown(ctx); err != nil {
		return fmt.Errorf("shutdown: %w", err)
	}
	return nil
}

func portFilePath() (string, error) {
	home, err := os.UserHomeDir()
	if err != nil {
		return "", fmt.Errorf("home dir: %w", err)
	}
	return filepath.Join(home, "Library", "Application Support", "cubevox", "port"), nil
}

// liveInstance returns the URL of a running app named by the port file, if it answers.
func liveInstance(portFile string) (string, bool) {
	data, err := os.ReadFile(portFile)
	if err != nil {
		return "", false
	}
	n, err := strconv.Atoi(strings.TrimSpace(string(data)))
	if err != nil {
		return "", false
	}
	url := fmt.Sprintf("http://127.0.0.1:%d", n)
	client := http.Client{Timeout: 2 * time.Second}
	resp, err := client.Get(url + "/api/state")
	if err != nil {
		return "", false
	}
	defer resp.Body.Close()
	return url, resp.StatusCode == http.StatusOK
}

func writePortFile(path string, port int) error {
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return fmt.Errorf("port file dir: %w", err)
	}
	if err := os.WriteFile(path, []byte(strconv.Itoa(port)+"\n"), 0o644); err != nil {
		return fmt.Errorf("write port file: %w", err)
	}
	return nil
}

func removePortFile(path string) {
	if err := os.Remove(path); err != nil && !errors.Is(err, os.ErrNotExist) {
		log.Printf("remove port file: %v", err)
	}
}

func openBrowser(url string, skip bool) error {
	if skip {
		return nil
	}
	if err := exec.Command("open", url).Run(); err != nil {
		return fmt.Errorf("open %s: %w", url, err)
	}
	return nil
}
