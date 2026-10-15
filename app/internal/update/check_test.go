package update

import (
	"context"
	"net/http"
	"net/http/httptest"
	"testing"
)

const releaseBody = `{"tag_name":"v0.02.00","assets":[
 {"name":"install.sh","browser_download_url":"https://x/install.sh","size":5},
 {"name":"cubevox.bin","browser_download_url":"https://x/cubevox.bin","size":1234},
 {"name":"cubevox.app.tar.gz","browser_download_url":"https://x/app.tgz","size":99}]}`

func releaseServer(t *testing.T, status int, body string) (*httptest.Server, *int) {
	t.Helper()
	hits := 0
	ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		hits++
		if r.Header.Get("User-Agent") == "" {
			t.Error("no User-Agent")
		}
		w.WriteHeader(status)
		w.Write([]byte(body))
	}))
	t.Cleanup(ts.Close)
	return ts, &hits
}

func TestCheck(t *testing.T) {
	cases := []struct {
		name, app, fw        string
		available, appB, fwB bool
	}{
		{"app behind", "v0.01.00", "v0.02.00", true, true, false},
		{"firmware behind", "v0.02.00", "v0.01.05-3-gabc1234", true, false, true},
		{"both behind", "v0.01.00", "v0.01.00", true, true, true},
		{"same tag", "v0.02.00", "v0.02.00", false, false, false},
		{"between tags", "v0.02.00-4-gdeadbee", "v0.02.00-1-gabc", false, false, false},
		{"dev build, no box", "dev", "", false, false, false},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			ts, hits := releaseServer(t, 200, releaseBody)
			info, err := Checker{URL: ts.URL}.Check(context.Background(), c.app, c.fw)
			if err != nil {
				t.Fatal(err)
			}
			if *hits != 1 {
				t.Errorf("%d requests, want 1", *hits)
			}
			if info.Available != c.available || info.AppBehind != c.appB || info.FWBehind != c.fwB {
				t.Errorf("got %+v", info)
			}
			if info.Tag != "v0.02.00" || info.BinURL != "https://x/cubevox.bin" || info.BinSize != 1234 ||
				info.AppURL != "https://x/app.tgz" || info.AppSize != 99 {
				t.Errorf("assets %+v", info)
			}
		})
	}
}

func TestCheckFailures(t *testing.T) {
	for name, c := range map[string]struct {
		status int
		body   string
	}{
		"rate limited": {403, `{"message":"rate limit"}`},
		"bad json":     {200, `{`},
		"no tag":       {200, `{"assets":[]}`},
	} {
		t.Run(name, func(t *testing.T) {
			ts, _ := releaseServer(t, c.status, c.body)
			if _, err := (Checker{URL: ts.URL}).Check(context.Background(), "v0.01.00", ""); err == nil {
				t.Fatal("want error")
			}
		})
	}
}

func TestCheckNetworkDown(t *testing.T) {
	ts, _ := releaseServer(t, 200, releaseBody)
	url := ts.URL
	ts.Close()
	if _, err := (Checker{URL: url}).Check(context.Background(), "v0.01.00", ""); err == nil {
		t.Fatal("want error")
	}
}

func TestForRecomputes(t *testing.T) {
	info := Info{Tag: "v0.02.00"}.For("v0.02.00", "")
	if info.Available {
		t.Fatalf("no box: %+v", info)
	}
	info = info.For("v0.02.00", "v0.01.00")
	if !info.FWBehind || !info.Available {
		t.Fatalf("old box: %+v", info)
	}
}
