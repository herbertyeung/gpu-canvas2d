#include "gpu2d/builtin_examples.hpp"

#include <array>

namespace gpu2d {
	namespace {

		constexpr std::string_view kPulse = R"HTML(<!doctype html>
<html><body>
<canvas id="c" width="960" height="540"></canvas>
<script>
const canvas = document.getElementById('c');
const ctx = canvas.getContext('2d');

function frame(time) {
  const pulse = 70 + Math.sin(time * 0.004) * 35;
  ctx.fillStyle = '#08111f';
  ctx.fillRect(0, 0, canvas.width, canvas.height);
  ctx.fillStyle = '#38bdf8';
  ctx.fillRect(canvas.width / 2 - pulse, canvas.height / 2 - pulse,
               pulse * 2, pulse * 2);
  requestAnimationFrame(frame);
}
requestAnimationFrame(frame);
</script>
</body></html>)HTML";

		constexpr std::string_view kBounce = R"HTML(<!doctype html>
<html><body>
<canvas id="c" width="960" height="540"></canvas>
<script>
const canvas = document.getElementById('c');
const ctx = canvas.getContext('2d');
let x = 70, y = 80, vx = 4.2, vy = 3.1;
const size = 48;

function frame() {
  ctx.shadowBlur = 0;
  ctx.fillStyle = '#07101d';
  ctx.fillRect(0, 0, canvas.width, canvas.height);
  x += vx; y += vy;
  if (x < 0 || x + size > canvas.width) vx = -vx;
  if (y < 0 || y + size > canvas.height) vy = -vy;
  ctx.fillStyle = '#f59e0b';
  ctx.shadowColor = '#fb923c';
  ctx.shadowBlur = 12;
  ctx.fillRect(x, y, size, size);
  requestAnimationFrame(frame);
}
requestAnimationFrame(frame);
</script>
</body></html>)HTML";

		constexpr std::string_view kOrbit = R"HTML(<!doctype html>
<html><body>
<canvas id="c" width="960" height="540"></canvas>
<script>
const canvas = document.getElementById('c');
const ctx = canvas.getContext('2d');

function hexagon(radius) {
  ctx.beginPath();
  for (let side = 0; side < 6; ++side) {
    const angle = side * Math.PI / 3;
    const x = Math.cos(angle) * radius;
    const y = Math.sin(angle) * radius;
    if (side === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
  }
  ctx.closePath();
}

function frame(time) {
  ctx.shadowBlur = 0;
  ctx.fillStyle = '#050816';
  ctx.fillRect(0, 0, canvas.width, canvas.height);
  ctx.save();
  ctx.translate(canvas.width / 2, canvas.height / 2);
  ctx.rotate(time * 0.0007);
  ctx.strokeStyle = '#a78bfa';
  ctx.lineWidth = 5;
  ctx.shadowColor = '#7c3aed';
  ctx.shadowBlur = 16;
  hexagon(120);
  ctx.stroke();
  ctx.restore();
  requestAnimationFrame(frame);
}
requestAnimationFrame(frame);
</script>
</body></html>)HTML";

		constexpr std::string_view kTrails = R"HTML(<!doctype html>
<html><body>
<canvas id="c" width="960" height="540"></canvas>
<script>
const canvas = document.getElementById('c');
const ctx = canvas.getContext('2d');
const points = [];
for (let i = 0; i < 18; ++i) {
  points.push({angle: i * Math.PI * 2 / 18, radius: 40 + i * 7});
}

ctx.fillStyle = '#020617';
ctx.fillRect(0, 0, canvas.width, canvas.height);
function frame(time) {
  ctx.globalCompositeOperation = 'source-over';
  ctx.shadowBlur = 0;
  ctx.fillStyle = 'rgba(2,6,23,0.10)';
  ctx.fillRect(0, 0, canvas.width, canvas.height);
  ctx.globalCompositeOperation = 'lighter';
  for (const point of points) {
    const angle = point.angle + time * 0.00055;
    const x = canvas.width / 2 + Math.cos(angle) * point.radius;
    const y = canvas.height / 2 + Math.sin(angle * 1.7) * point.radius;
    ctx.fillStyle = '#22d3ee';
    ctx.shadowColor = '#38bdf8';
    ctx.shadowBlur = 9;
    ctx.fillRect(x - 3, y - 3, 6, 6);
  }
  requestAnimationFrame(frame);
}
requestAnimationFrame(frame);
</script>
</body></html>)HTML";

		constexpr std::string_view kNeonLines = R"HTML(<!doctype html>
<html><body>
<canvas id="c" width="960" height="540"></canvas>
<script>
var w=c.width=window.innerWidth,h=c.height=window.innerHeight,ctx=c.getContext('2d');
var opts={len:20,count:50,baseTime:10,addedTime:10,dieChance:.05,spawnChance:1,
  sparkChance:.1,sparkDist:10,sparkSize:2,color:'hsl(hue,100%,light%)',baseLight:50,
  addedLight:10,shadowToTimePropMult:6,baseLightInputMultiplier:.01,
  addedLightInputMultiplier:.02,cx:w/2,cy:h/2,repaintAlpha:.04,hueChange:.1};
var tick=0,lines=[],dieX=w/2/opts.len,dieY=h/2/opts.len,baseRad=Math.PI*2/6;
ctx.fillStyle='black';ctx.fillRect(0,0,w,h);

function Line(){this.reset()}
Line.prototype.reset=function(){
  this.x=this.y=this.addedX=this.addedY=this.rad=0;
  this.lightInputMultiplier=opts.baseLightInputMultiplier+opts.addedLightInputMultiplier*Math.random();
  this.color=opts.color.replace('hue',tick*opts.hueChange);this.cumulativeTime=0;this.beginPhase();
};
Line.prototype.beginPhase=function(){
  this.x+=this.addedX;this.y+=this.addedY;this.time=0;
  this.targetTime=(opts.baseTime+opts.addedTime*Math.random())|0;
  this.rad+=baseRad*(Math.random()<.5?1:-1);
  this.addedX=Math.cos(this.rad);this.addedY=Math.sin(this.rad);
  if(Math.random()<opts.dieChance||this.x>dieX||this.x<-dieX||this.y>dieY||this.y<-dieY)this.reset();
};
Line.prototype.step=function(){
  ++this.time;++this.cumulativeTime;if(this.time>=this.targetTime)this.beginPhase();
  var prop=this.time/this.targetTime,wave=Math.sin(prop*Math.PI/2),
      x=this.addedX*wave,y=this.addedY*wave;
  ctx.shadowBlur=prop*opts.shadowToTimePropMult;
  ctx.fillStyle=ctx.shadowColor=this.color.replace('light',opts.baseLight+opts.addedLight*Math.sin(this.cumulativeTime*this.lightInputMultiplier));
  ctx.fillRect(opts.cx+(this.x+x)*opts.len,opts.cy+(this.y+y)*opts.len,2,2);
  if(Math.random()<opts.sparkChance)ctx.fillRect(opts.cx+(this.x+x)*opts.len+Math.random()*opts.sparkDist*(Math.random()<.5?1:-1)-opts.sparkSize/2,opts.cy+(this.y+y)*opts.len+Math.random()*opts.sparkDist*(Math.random()<.5?1:-1)-opts.sparkSize/2,opts.sparkSize,opts.sparkSize);
};
function loop(){
  requestAnimationFrame(loop);++tick;ctx.globalCompositeOperation='source-over';ctx.shadowBlur=0;
  ctx.fillStyle='rgba(0,0,0,alp)'.replace('alp',opts.repaintAlpha);ctx.fillRect(0,0,w,h);
  ctx.globalCompositeOperation='lighter';
  if(lines.length<opts.count&&Math.random()<opts.spawnChance)lines.push(new Line);
  lines.map(function(line){line.step()});
}
loop();
window.addEventListener('resize',function(){
  w=c.width=window.innerWidth;h=c.height=window.innerHeight;ctx.fillStyle='black';ctx.fillRect(0,0,w,h);
  opts.cx=w/2;opts.cy=h/2;dieX=w/2/opts.len;dieY=h/2/opts.len;
});
</script>
</body></html>)HTML";

		// Adapted from "Long shadow" by Mladen Stanojevic:
		// https://codepen.io/mladen___/pen/gbvqBo
		constexpr std::string_view kLongShadow = R"HTML(<!doctype html>
<html>
<head>
<style>
html { height: 100%; }
body {
  margin: 0;
  padding: 0;
  height: 100%;
  overflow: hidden;
  cursor: none;
}
#canvas {
  background-color: #2c343f;
  width: 100%;
  height: 100%;
}
</style>
</head>
<body>
<canvas id="canvas"></canvas>
<script>
// Source: https://codepen.io/mladen___/pen/gbvqBo
var c = document.getElementById("canvas");
var ctx = c.getContext("2d");

function resize() {
  var box = c.getBoundingClientRect();
  c.width = box.width;
  c.height = box.height;
}

var light = { x: 160, y: 200 };
var colors = ["#f5c156", "#e6616b", "#5cd3ad"];

function drawLight() {
  ctx.beginPath();
  ctx.arc(light.x, light.y, 1000, 0, 2 * Math.PI);
  var gradient = ctx.createRadialGradient(light.x, light.y, 0, light.x, light.y, 1000);
  gradient.addColorStop(0, "#3b4654");
  gradient.addColorStop(1, "#2c343f");
  ctx.fillStyle = gradient;
  ctx.fill();

  ctx.beginPath();
  ctx.arc(light.x, light.y, 20, 0, 2 * Math.PI);
  gradient = ctx.createRadialGradient(light.x, light.y, 0, light.x, light.y, 5);
  gradient.addColorStop(0, "#fff");
  gradient.addColorStop(1, "#3b4654");
  ctx.fillStyle = gradient;
  ctx.fill();
}

function Box() {
  this.half_size = Math.floor(Math.random() * 50 + 1);
  this.x = Math.floor(Math.random() * c.width + 1);
  this.y = Math.floor(Math.random() * c.height + 1);
  this.r = Math.random() * Math.PI;
  this.shadow_length = 2000;
  this.color = colors[Math.floor(Math.random() * colors.length)];

  this.getDots = function() {
    var full = Math.PI * 2 / 4;
    var p1 = { x: this.x + this.half_size * Math.sin(this.r),
               y: this.y + this.half_size * Math.cos(this.r) };
    var p2 = { x: this.x + this.half_size * Math.sin(this.r + full),
               y: this.y + this.half_size * Math.cos(this.r + full) };
    var p3 = { x: this.x + this.half_size * Math.sin(this.r + full * 2),
               y: this.y + this.half_size * Math.cos(this.r + full * 2) };
    var p4 = { x: this.x + this.half_size * Math.sin(this.r + full * 3),
               y: this.y + this.half_size * Math.cos(this.r + full * 3) };
    return { p1: p1, p2: p2, p3: p3, p4: p4 };
  };

  this.rotate = function() {
    var speed = (60 - this.half_size) / 20;
    this.r += speed * 0.002;
    this.x += speed;
    this.y += speed;
  };

  this.draw = function() {
    var dots = this.getDots();
    ctx.beginPath();
    ctx.moveTo(dots.p1.x, dots.p1.y);
    ctx.lineTo(dots.p2.x, dots.p2.y);
    ctx.lineTo(dots.p3.x, dots.p3.y);
    ctx.lineTo(dots.p4.x, dots.p4.y);
    ctx.fillStyle = this.color;
    ctx.fill();
    if (this.y - this.half_size > c.height) this.y -= c.height + 100;
    if (this.x - this.half_size > c.width) this.x -= c.width + 100;
  };

  this.drawShadow = function() {
    var dots = this.getDots();
    var angles = [];
    var points = [];
    for (dot in dots) {
      var angle = Math.atan2(light.y - dots[dot].y, light.x - dots[dot].x);
      var endX = dots[dot].x + this.shadow_length * Math.sin(-angle - Math.PI / 2);
      var endY = dots[dot].y + this.shadow_length * Math.cos(-angle - Math.PI / 2);
      angles.push(angle);
      points.push({ endX: endX, endY: endY,
                    startX: dots[dot].x, startY: dots[dot].y });
    }
    for (var i = points.length - 1; i >= 0; i--) {
      var n = i == 3 ? 0 : i + 1;
      ctx.beginPath();
      ctx.moveTo(points[i].startX, points[i].startY);
      ctx.lineTo(points[n].startX, points[n].startY);
      ctx.lineTo(points[n].endX, points[n].endY);
      ctx.lineTo(points[i].endX, points[i].endY);
      ctx.fillStyle = "#2c343f";
      ctx.fill();
    }
  };
}

var boxes = [];
function draw() {
  ctx.clearRect(0, 0, c.width, c.height);
  drawLight();
  for (var i = 0; i < boxes.length; i++) {
    boxes[i].rotate();
    boxes[i].drawShadow();
  }
  for (var i = 0; i < boxes.length; i++) {
    collisionDetection(i);
    boxes[i].draw();
  }
  requestAnimationFrame(draw);
}

resize();
draw();
while (boxes.length < 14) boxes.push(new Box());

window.onresize = resize;
c.onmousemove = function(e) {
  light.x = e.offsetX == undefined ? e.layerX : e.offsetX;
  light.y = e.offsetY == undefined ? e.layerY : e.offsetY;
};

function collisionDetection(b) {
  for (var i = boxes.length - 1; i >= 0; i--) {
    if (i != b) {
      var dx = boxes[b].x + boxes[b].half_size - (boxes[i].x + boxes[i].half_size);
      var dy = boxes[b].y + boxes[b].half_size - (boxes[i].y + boxes[i].half_size);
      var d = Math.sqrt(dx * dx + dy * dy);
      if (d < boxes[b].half_size + boxes[i].half_size) {
        boxes[b].half_size = boxes[b].half_size > 1 ? boxes[b].half_size -= 1 : 1;
        boxes[i].half_size = boxes[i].half_size > 1 ? boxes[i].half_size -= 1 : 1;
      }
    }
  }
}
</script>
</body>
</html>)HTML";

		constexpr std::array<BuiltinExample, 6> kExamples{ {
			{"01 - Pulse | fillRect + RAF", kPulse, false},
			{"02 - Bounce | position + velocity", kBounce, true},
			{"03 - Orbit | transforms + path", kOrbit, true},
			{"04 - Trails | persistent alpha fade", kTrails, true},
			{"05 - Neon lines | HSL + shadows + objects", kNeonLines, true},
			{"06 - Long shadow | CodePen + CSS + gradients", kLongShadow, true},
		} };

	}  // namespace

	std::span<const BuiltinExample> builtinExamples() noexcept { return kExamples; }

}  // namespace gpu2d