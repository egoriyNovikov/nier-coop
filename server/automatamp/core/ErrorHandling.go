package core

import "github.com/codecat/go-libs/log"

func check(err error) {
	if err != nil {
		panic(err)
	}
}

func handlepanic() {
	if a := recover(); a != nil {
		log.Error("Recovered from a panic: %v", a)
	}
}
