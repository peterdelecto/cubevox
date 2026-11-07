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

// AppAsset is the release asset that holds the app bundle.
const AppAsset = "cubevox.app.tar.gz"

const checkTimeout = 10 * time.Second

// Info is the newest release and whether this app is behind it.
type Info struct {
	Tag     string
	AppURL  string
	AppSize int64

	Available bool // this app is older than Tag
}

// For returns i with Available set for the given app version. An unparsable version (a "dev"
// build) is never behind.
func (i Info) For(app string) Info {
	i.Available = Newer(i.Tag, app)
	return i
}

// Checker fetches the latest release. The zero value talks to GitHub.
type Checker struct {
	URL    string       // empty means ReleaseURL
	Client *http.Client // nil means a client with a 10 s timeout
}

// Check fetches the latest release with the default Checker.
func Check(ctx context.Context, current string) (Info, error) {
	return Checker{}.Check(ctx, current)
}

type releaseJSON struct {
	TagName string `json:"tag_name"`
	Assets  []struct {
		Name string `json:"name"`
		URL  string `json:"browser_download_url"`
		Size int64  `json:"size"`
	} `json:"assets"`
}

// Check makes one GET for the latest release and compares its tag with the app version current.
func (c Checker) Check(ctx context.Context, current string) (Info, error) {
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
		if a.Name == AppAsset {
			info.AppURL, info.AppSize = a.URL, a.Size
		}
	}
	return info.For(current), nil
}
