package main

import (
	"bytes"
	"encoding/binary"
	"errors"
	"flag"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"io"
	"os"
	"path"
	"sync"
)

const (
	OP6TOffset int64 = 0
)

type OP6TLogoHeader struct {
	Header       [8]uint8
	Blank        [24]uint8
	Width        uint32
	Height       uint32
	LengthOfData uint32
	Special      uint32
	Offsets      [84]uint32
	Name         [64]uint8
	MetaData     [3648]uint8
}

type Config struct {
	Unpack   bool
	Readinfo bool
	Input    string
	Picdir   string
	Output   string
}

func (header *OP6TLogoHeader) Read(reader io.Reader) error {
	binary.Read(reader, binary.LittleEndian, header)
	if header.Special == 1 && header.Offsets[0] == 0 {
		return nil
	}
	return errors.New("File format seems not a oneplus 6t splash image.")
}

func (header *OP6TLogoHeader) Print(reader io.ReadWriteSeeker) error {
	strdup := func(s []byte) string {
		for index, current := range s {
			if current == 0 {
				return string(s[:index])
			}
		}
		return string(s)
	}
	reader.Seek(0, io.SeekStart)
	fmt.Printf("INDEX\tOFFSET\tWIDTH\tHEIGHT\tLENGTH\tSPECIAL\t%s:[%s]\n", "METADATA", "NAME")
	for index, offset := range header.Offsets {

		hdr := OP6TLogoHeader{}

		reader.Seek(int64(offset), io.SeekStart)
		//println("seek", offset)
		binary.Read(reader, binary.LittleEndian, &hdr)
		//if err != nil {
		//	return err
		//}

		var name string
		if hdr.Name[0] == 0 {
			name = "None"
		} else {
			name = strdup(hdr.Name[:])
		}
		fmt.Printf("%5d\t%06X\t%5d\t%6d\t%6d\t%7d\t%s",
			index, offset, hdr.Width, hdr.Height, hdr.LengthOfData, hdr.Special, hdr.MetaData)
		fmt.Printf(":[%s]\n", name)
	}
	return nil
}

func (header *OP6TLogoHeader) Unpack(reader io.ReadWriteSeeker, outdir string) error {
	strdup := func(s []byte) string {
		for index, current := range s {
			if current == 0 {
				return string(s[:index])
			}
		}
		return string(s)
	}

	hdr := OP6TLogoHeader{}
	saveto := func(w int, h int, rle []byte, to string, wg *sync.WaitGroup) {
		defer wg.Done()
		rawdata := Rle2Raw(rle)

		img := image.NewRGBA(image.Rect(0, 0, w, h))
		var r uint8
		var g uint8
		var b uint8
		var a uint8 = 255
		var pix int = 0
		for y := 0; y < h; y++ {
			for x := 0; x < w; x++ {
				pix = (y*w + x) * 3
				b = rawdata[pix]
				g = rawdata[pix+1]
				r = rawdata[pix+2]
				img.SetRGBA(x, y, color.RGBA{r, g, b, a})
			}
		}
		out, err := os.Create(to)
		if err != nil {
			println("Error: ", err.Error())
			return
		}
		png.Encode(out, img)
	}

	_, err := os.Stat(outdir)
	if os.IsNotExist(err) {
		os.MkdirAll(outdir, 0777)
	}

	var wg sync.WaitGroup
	for index, offset := range header.Offsets {
		reader.Seek(int64(offset), io.SeekStart)
		binary.Read(reader, binary.LittleEndian, &hdr)
		fmt.Printf("%-15s [%s]\n", "Parsing", hdr.Name)
		fmt.Printf("\tNUM:\t%d\n", index)
		fmt.Printf("\tOFFSET:\t%d\n", offset)
		fmt.Printf("\tWIDTH:\t%d\n", hdr.Width)
		fmt.Printf("\tHEIGHT:\t%d\n", hdr.Height)
		fmt.Printf("\tRLESZ:\t%d\n", hdr.LengthOfData)
		fmt.Printf("\tMetadata\t%s\n", hdr.MetaData)

		if hdr.LengthOfData == 0 {
			fmt.Printf("\t\tSKIP VOID BUFFER ...\n")
			continue
		}

		rledata := make([]byte, hdr.LengthOfData)
		_, err := reader.Read(rledata)
		if err != nil {
			fmt.Println("Error: ", err.Error())
			continue
		}
		wg.Add(1)
		go saveto(int(hdr.Width), int(hdr.Height), rledata, path.Join(outdir, strdup(hdr.Name[:])+".png"), &wg)
	}
	wg.Wait()
	return nil
}

func (header *OP6TLogoHeader) Repack(reader io.ReadWriteSeeker, picdir string, outfile string) error {
	strdup := func(s []byte) string {
		for index, current := range s {
			if current == 0 {
				return string(s[:index])
			}
		}
		return string(s)
	}
	offupper := func(off uint32) uint32 {
		if off%0x1000 == 0 {
			return off
		}
		return ((off >> 0xc) + 1) << 0xc
	}
	rgba2rgb := func(img image.Image, width int, height int) []byte {
		buf := new(bytes.Buffer)
		var ur, ug, ub uint8
		for y := 0; y < height; y++ {
			for x := 0; x < width; x++ {
				b, g, r, _ := img.At(x, y).RGBA()
				ur = uint8(b >> 8)
				ug = uint8(g >> 8)
				ub = uint8(r >> 8)
				//if r != 0 && g != 0 && b != 0 {
				//	fmt.Println(ur, ug, ub)
				//}
				buf.WriteByte(ur)
				buf.WriteByte(ug)
				buf.WriteByte(ub)
			}
		}
		return buf.Bytes()
	}
	//writeto := func() {}
	//offsets := make([]uint32, len(header.Offsets))
	hdr := OP6TLogoHeader{}
	off := uint32(0)

	result := new(bytes.Buffer)
	for index, offset := range header.Offsets {
		if off == 0 {
			header.Offsets[index] = off
		} else {
			off = offupper(off)
			header.Offsets[index] = off
		}
		reader.Seek(int64(offset), io.SeekStart)
		binary.Read(reader, binary.LittleEndian, &hdr)

		fmt.Printf("Parsing [%4d] [%24s] [%s] ... ", index, string(hdr.MetaData[:]), hdr.Name)

		if hdr.LengthOfData == 0 {
			fmt.Printf("Skip\n")
			off += uint32(binary.Size(hdr))
			binary.Write(result, binary.LittleEndian, &hdr)
			continue
		}
		pngfd, err := os.Open(path.Join(picdir, strdup(hdr.Name[:])+".png"))
		if os.IsExist(err) {
			fmt.Printf("Faild:\n\tCannot find this png file at %s\n", picdir)
			return errors.New("Pic does not exist !")
		}
		img, err := png.Decode(pngfd)
		if err != nil {
			fmt.Println("Error: ", err.Error())
			return err
		}
		imginfo := img.Bounds().Max
		hdr.Width = uint32(imginfo.X)
		hdr.Height = uint32(imginfo.Y)

		rawdata := rgba2rgb(img, imginfo.X, imginfo.Y)
		rledata := Raw2Rle(rawdata)

		if index == 0 {
			header.Width = hdr.Width
			header.Height = hdr.Height
			header.LengthOfData = hdr.LengthOfData
		}

		//print(len(rledata))
		fmt.Printf("Done\n")

		off += uint32(binary.Size(hdr))
		if hdr.LengthOfData != 0 {
			off += uint32(len(rledata))
		}

		binary.Write(result, binary.LittleEndian, &hdr)
		if hdr.LengthOfData != 0 {
			binary.Write(result, binary.LittleEndian, &rledata)
		}

		for j := uint32(0); j < offupper(off)-off; j++ { // Padding
			result.WriteByte(0)
		}

	}

	ofd, err := os.Create(outfile)
	if err != nil {
		fmt.Println("Error: ", err.Error())
		return err
	}
	defer ofd.Close()
	// update offsets
	for index, offset := range header.Offsets {
		println(index, offset)
	}
	binary.Write(ofd, binary.LittleEndian, header)
	ofd.Write(result.Bytes()[binary.Size(hdr):])

	return nil
}

// Return raw data from run length encoding
// 00 04 = [00 00 00 00]
func Rle2Raw(data []byte) []byte {
	buf := new(bytes.Buffer)
	var sz uint8
	var b uint8
	for i := 0; i < len(data); i++ {
		if i&1 == 1 {
			sz = data[i]
			for j := uint8(0); j < sz; j++ {
				buf.WriteByte(b)
			}
		} else {
			b = data[i]
		}
	}
	return buf.Bytes()
}

func Raw2Rle(data []byte) []byte {
	var last byte
	buf := new(bytes.Buffer)
	count := uint8(0)
	length := len(data)
	for i := 0; i < length; i++ {
		if i == 0 {
			last = data[i]
			count++
		} else {
			if data[i] == last {
				if count == 255 {
					buf.WriteByte(last)
					buf.WriteByte(count)
					count = 1
				} else {
					count++
				}
			} else {
				buf.WriteByte(last)
				buf.WriteByte(count)
				count = 1
			}
			if i == length-1 {
				last = data[i]
				buf.WriteByte(last)
				buf.WriteByte(count)
			}
		}
		last = data[i]
	}
	return buf.Bytes()
}

func main() {

	config := Config{}
	flag.BoolVar(&config.Unpack, "x", false, "Provide -x can unpack, or repack")
	flag.StringVar(&config.Input, "i", "", "Default is LOGO.img")
	flag.StringVar(&config.Output, "o", "new-logo.img", "Default output is new-logo.img")
	flag.StringVar(&config.Picdir, "p", "pic", "Default is pic, the dir where pic extract for")
	flag.BoolVar(&config.Readinfo, "r", false, "Default is false, spec this read image info only")

	flag.Parse()

	if config.Input == "" {
		fmt.Println("Oneplus 6t LOGO image unpack/repack tool")
		flag.Usage()
		fmt.Println("Use by your own risk!")
		return
	}

	fd, err := os.Open(config.Input)
	if os.IsNotExist(err) {
		fmt.Println(err.Error())
		return
	}

	ophdr := OP6TLogoHeader{}
	ophdr.Read(fd)
	if config.Readinfo {
		ophdr.Print(fd)
		return
	}

	if config.Unpack {
		ophdr.Unpack(fd, config.Picdir)
	} else {
		ophdr.Repack(fd, config.Picdir, config.Output)
	}
}
