package update

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"time"
)

// ReleaseURL is the latest-release endpoint of the public repo (no token needed).
const ReleaseURL = "https://api.github.com/repos/peterdelecto/cubevox/releases/latest"

// Release asset names.
const (
	FirmwareAsset = "cubevox.bin"
	AppAsset      = "cubevox.app.tar.gz"
)

const checkTimeout = 10 * time.Second

// Info is the newest release and how it compares with this app and the box.
type Info struct {
	Tag     string
	BinURL  string
	BinSize int64
	AppURL  string
	AppSize int64

	AppBehind bool // this app is older than Tag
	FWBehind  bool // the box firmware is older than Tag
	Available bool // either one is behind
}

// For returns i with the behind flags recomputed for the given app and firmware versions. The
// release is fetched once per page open, but the box may connect later, so the page state
// recomputes the flags on every poll. An empty or unparsable version is never behind.
func (i Info) For(app, fw string) Info {
	i.AppBehind = Newer(i.Tag, app)
	i.FWBehind = Newer(i.Tag, fw)
	i.Available = i.AppBehind || i.FWBehind
	return i
}

// Checker fetches the latest release. The zero value talks to GitHub.
type Checker struct {
	URL    string       // empty means ReleaseURL
	Client *http.Client // nil means a client with a 10 s timeout
}

// Check fetches the latest release with the default Checker.
func Check(ctx context.Context, current, fw string) (Info, error) {
	return Checker{}.Check(ctx, current, fw)
}

type releaseJSON struct {
	TagName string `json:"tag_name"`
	Assets  []struct {
		Name string `json:"name"`
		URL  string `json:"browser_download_url"`
		Size int64  `json:"size"`
	} `json:"assets"`
}

// Check makes one GET for the latest release and compares its tag with the app version current
// and the box firmware version fw (either may be empty).
func (c Checker) Check(ctx context.Context, current, fw string) (Info, error) {
	url, client := c.URL, c.Client
	if url == "" {
		url = ReleaseURL
	}
	if client == nil {
		client = &http.Client{Timeout: checkTimeout}
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return Info{}, fmt.Errorf("release request: %w", err)
	}
	req.Header.Set("User-Agent", "cubevox-app/"+current)
	req.Header.Set("Accept", "application/vnd.github+json")
	resp, err := client.Do(req)
	if err != nil {
		return Info{}, fmt.Errorf("fetch latest release: %w", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return Info{}, fmt.Errorf("fetch latest release: %s", resp.Status)
	}
	var rel releaseJSON
	if err := json.NewDecoder(io.LimitReader(resp.Body, 1<<20)).Decode(&rel); err != nil {
		return Info{}, fmt.Errorf("parse latest release: %w", err)
	}
	if rel.TagName == "" {
		return Info{}, fmt.Errorf("latest release has no tag")
	}
	info := Info{Tag: rel.TagName}
	for _, a := range rel.Assets {
		switch a.Name {
		case FirmwareAsset:
			info.BinURL, info.BinSize = a.URL, a.Size
		case AppAsset:
			info.AppURL, info.AppSize = a.URL, a.Size
		}
	}
	return info.For(current, fw), nil
}
