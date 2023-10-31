GO = go
LDFLAGS = "-s -w"

src = op6tlogo.go

.PHONY: all

all: op6tlogo

op6tlogo:
  @$(GO) build -ldflags $(LDFLAGS) $(src)
