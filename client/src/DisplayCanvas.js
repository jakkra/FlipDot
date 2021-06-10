import React, {Component} from 'react';

import { Col, Row } from 'react-bootstrap';

import Toolbar from './Toolbar';
import {displaySize} from './config';
import { ToastContainer, toast } from 'react-toastify';
import 'gifler';


const Modes = {
  DRAW: 'draw',
  VIDEO: 'video',
}

export default class DrawArea extends Component {
  constructor() {
    super();

    this.state = {
      mode: Modes.DRAW,
      isDrawing: false,
      wsOpen: false,
      wsClosing: false,
      wsConnecting: false,
    };

    this.width = displaySize.width;
    this.height = displaySize.height;
    this.type = '2d';
    this.ip = '192.168.1.38:81';
    this.drawingColor = {
      hex: "#0000FF"
    };
    this.drawWidth = 1;

    this.displayChanged = false;

    this.handleMouseDown = this.handleMouseDown.bind(this);
    this.handleMouseMove = this.handleMouseMove.bind(this);
    this.handleMouseUp = this.handleMouseUp.bind(this);
    this.resizeCanvas = this.resizeCanvas.bind(this);
    this.sendCanvasData = this.sendCanvasData.bind(this);
    this.connect = this.connect.bind(this);
    this.drawVideoToCanvas = this.drawVideoToCanvas.bind(this);
    this.connectDisconnectClicked = this.connectDisconnectClicked.bind(this);
    this.getVideo = this.getVideo.bind(this);
    this.changeDisplayMode = this.changeDisplayMode.bind(this);
    this.clearCanvas = this.clearCanvas.bind(this);
    this.handleDrawColorChange = this.handleDrawColorChange.bind(this);
    this.handleDrawWidthChange = this.handleDrawWidthChange.bind(this);
    this.drawPixel = this.drawPixel.bind(this);
    this.handleDisplayImage = this.handleDisplayImage.bind(this);
    this.handleDrawText = this.handleDrawText.bind(this);
    this.onDrawFrame = this.onDrawFrame.bind(this);
  }

  componentDidMount() {
    document.addEventListener("mouseup", this.handleMouseUp);
    window.addEventListener("resize", this.resizeCanvas);
    this.setupCanvas();
    this.resizeCanvas();
    //this.timerID = setInterval(() => this.sendCanvasData(), 250);
  }

  componentWillUnmount() {
    document.removeEventListener("mouseup", this.handleMouseUp);
    document.removeEventListener("touchend", this.handleMouseUp);
    clearInterval(this.refreshDisplayTimer);
  }

  setupCanvas() {
    this.refs.canvas.width = this.width;
    this.refs.canvas.height = this.height;
    // Needed to be set like this for mouseEvent.preventDefault() to work
    this.refs.canvas.ontouchstart = this.handleMouseDown;
  }

  drawPixel(point) {
    const ctx = this.refs.canvas.getContext('2d');
    ctx.fillStyle = this.drawingColor.hex;
    ctx.fillRect(Math.round(point.x), Math.round(point.y), this.drawWidth, this.drawWidth);
    this.displayChanged = true;
  }

  handleMouseDown(mouseEvent) {
    mouseEvent.preventDefault(); // Avoids scrolling page when drawing
    const point = this.relativeCoordinatesForEvent(mouseEvent);
    this.drawPixel(point);

    this.setState({
      isDrawing: true
    });
  }

  handleMouseMove(mouseEvent) {
    if (!this.state.isDrawing) {
      return;
    }
    const point = this.relativeCoordinatesForEvent(mouseEvent);
    this.drawPixel(point);
  }

  handleMouseUp() {
    this.setState({ isDrawing: false });
  }

  relativeCoordinatesForEvent(mouseEvent) {
    let eventX;
    let eventY;

    if (mouseEvent.type === 'touchmove') {
      eventX = mouseEvent.touches[0].clientX;
      eventY = mouseEvent.touches[0].clientY;
    } else {
      eventX = mouseEvent.clientX;
      eventY = mouseEvent.clientY;
    }

    const canvas = this.refs.canvas;
    const rect = canvas.getBoundingClientRect(),
      scaleX = canvas.width / rect.width,
      scaleY = canvas.height / rect.height;

    return {
      x: (eventX - rect.left) * scaleX,
      y: (eventY - rect.top) * scaleY
    }
  }

  resizeCanvas() {
    const scaleW = Math.floor((window.innerWidth) / this.width);
    const scaleH = Math.floor((window.innerHeight - window.innerHeight / 10) / this.height);

    const scale = (scaleH < scaleW) ? scaleH : scaleW;
    const newWidth = this.width * scale;
    const newHeight = this.height * scale;

    this.refs.canvas.style.width = `${newWidth}px`;
    this.refs.canvas.style.height = `${newHeight}px`;

    this.stageResized = true;
  }

  sendCanvasData() {
    if (!this.stageResized) {
      this.resizeCanvas(this.refs.canvas);
    }

    if (!this.state.wsOpen || this.state.wsClosing) {
      return;
    }

    const buffer = new ArrayBuffer(this.width * this.height);
    const bytearray = new Uint8Array(buffer, 0, this.width * this.height);

    let context;
    if (this.type === '2d') {
      context = this.refs.canvas.getContext('2d');
    } else if (this.type === 'webgl') {
      context = this.refs.canvas.getContext('webgl', {
        antialias: false,
        depth: false,
      });
    }

    let index = 0;
    for (let y = 0; y < this.height; y++) {
      for (let x = 0; x < this.width; x++) {
        let pixel;

        if (this.type === '2d') {
          pixel = context.getImageData(x, y, 1, 1).data;
        }

        bytearray[index++] = pixel[0];
      }
    }
    //if (this.displayChanged === true) {
      this.ws.send(bytearray);
      this.displayChanged = false;
    //}
  }

  connect(ipAddress) {
    this.ws = new WebSocket(`ws://${ipAddress}`);
    this.ws.binaryType = 'arraybuffer';

    if (this.state.wsClosing || this.state.wsOpen) {
      console.error('ws is in closing or closed state');
      return;
    }

    this.clearCanvas();

    this.setState({
      wsConnecting: true,
      wsClosing: false,
    });

    this.ws.onopen = () => {
      console.log('WebSocket open');
      this.setState({
        wsOpen: true,
        wsConnecting: false,
        wsClosing: false,
      });
    };

    this.ws.onclose = () => {
      console.log('WebSocket close');
      this.setState({
        wsOpen: false,
        wsConnecting: false,
        wsClosing: false,
      });    
    };

    this.ws.onerror = (evt) => {
      console.log(evt);
      this.setState({
        wsOpen: false,
        wsConnecting: false,
        wsClosing: false
      });
    };

    this.ws.onmessage = (evt) => {
      console.log(`WS message: ${evt.data}`);
    };
  }

  getVideo() {
  navigator.mediaDevices.getUserMedia({ video: true, audio: false })
    .then(localMediaStream => {
      try {
        this.refs.video.srcObject = localMediaStream;
      } catch (error) {
        this.refs.video.src = window.URL.createObjectURL(localMediaStream);
      }
      this.refs.video.play();
    })
    .catch(err => {
      toast.error('This device does not allow or support usage of webcamera.')
      console.error(`No access to webcam`, err);
      this.changeDisplayMode();
    });
  }

  drawVideoToCanvas() {
    const context = this.refs.canvas.getContext('2d');
    context.drawImage(this.refs.video, 0, 0, this.width, this.height);
    this.sendCanvasData();
  }

  connectDisconnectClicked(ipAddress) {
    if (this.state.wsOpen) {
      this.setState({
        wsClosing: true,
      });
      this.clearCanvas();
      this.ws.close();
    } else {
      this.connect(ipAddress);
    }
  }

  changeDisplayMode() {
    let mode = Modes.DRAW;
    if (this.state.mode === Modes.DRAW) {
      mode = Modes.VIDEO;
    }
    this.setState({
      mode: mode,
    });
    
    clearInterval(this.timerID);
    this.clearCanvas();

    if (mode === Modes.DRAW) {
      this.refs.video.pause();
      //this.timerID = setInterval(() => this.sendCanvasData(), 250);
    } else {
      this.getVideo();
      this.refs.video.addEventListener('canplay', () => {
        this.timerID = setInterval(this.drawVideoToCanvas, 100);
      });
    }
  }

  handleDrawColorChange(color) {
    this.drawingColor = color;
  }

  handleDrawWidthChange(width)  {
    this.drawWidth = width;
  }

  onDrawFrame(ctx, frame) {
    // update canvas size
    //canvas.width = frame.width;
    //canvas.height = frame.height;
    // update canvas that we are using for Konva.Image
    //ctx.drawImage(frame.buffer, 0, 0);
    frame.delay = 5;
    ctx.drawImage(frame.buffer, 0, 0, this.width, this.height);
    this.sendCanvasData();
    console.log("send")
  }

  handleDisplayImage(url) {
    const context = this.refs.canvas.getContext('2d');
    window.gifler(url).frames(this.refs.canvas, this.onDrawFrame);
    //const image = new Image();
    //image.crossOrigin = "Anonymous";
    //image.src = url;
    //image.onload = () => {
    //  context.drawImage(image, 0, 0, this.width, this.height);
    //};
  }

  getLines(ctx, text, maxWidth) {
    var words = text.split(" ");
    var lines = [];
    var currentLine = words[0]

    for (var i = 1; i < words.length; i++) {
        var word = words[i]
        var width = ctx.measureText(currentLine + " " + word).width;
        if (width < maxWidth) {
            currentLine += " " + word;
        } else {
            lines.push(currentLine);
            currentLine = word;
        }
    }
    lines.push(currentLine);
    return lines;
}

  handleDrawText(text) {
    const context = this.refs.canvas.getContext('2d');
    const lines = this.getLines(context, text.toUpperCase(), this.width)
    console.log(lines);
    context.font = "8px Verdana";
    context.fillStyle = this.drawingColor.hex;
    context.textAlign = "left";
    for (var i = 0; i < lines.length; i++) {
      context.fillText(lines[i], 0, (i + 1) * 7);
    }
  }

  clearCanvas() {
    const context = this.refs.canvas.getContext('2d');
    context.clearRect(0, 0, this.width, this.height);
    this.displayChanged = true;
  }

  render() {
    return (
      <div className="drawArea" ref="drawArea" >
        <ToastContainer autoClose={5000} position="top-center" closeOnClick/>
        <Col className="Container">
          <Row xs={10}>
            <canvas
            ref="canvas"
            width={64}
            height={32}
            style={{letterSpacing: 1}}
            onTouchMove={this.handleMouseMove}
            onMouseDown={this.handleMouseDown}
            onMouseMove={this.handleMouseMove}/>
          </Row>
          <Row>
            <video ref="video"/>
          </Row>
          <Row xs={2}>
            <Toolbar
              isConnected={this.state.wsOpen}
              isConnecting={this.state.wsConnecting}
              onConnectDisconnect={this.connectDisconnectClicked}
              onModeSwitch={this.changeDisplayMode}
              onDrawWidthChange={this.handleDrawWidthChange}
              onColorChange={this.handleDrawColorChange}
              onErase={this.clearCanvas}
              onImageUrlProvided={this.handleDisplayImage}
              onDisplayText={this.handleDrawText} />
          </Row>
        </Col>
      </div>
    );
  }
}


