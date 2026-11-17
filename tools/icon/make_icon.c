// Draws the nodeTrix app icon (48x48 SMDH icon) with CoreGraphics:
// the white ring with a red cube, a yellow sphere and a blue cylinder on it.
//   make -C tools/icon        -> sequencer/icon.png (+ icon-preview.png at 8x)
#include <ApplicationServices/ApplicationServices.h>
#include <ImageIO/ImageIO.h>
#include <math.h>
#include <stdio.h>

static void rgb(CGContextRef c, double r, double g, double b, double a) {
    CGContextSetRGBStrokeColor(c, r / 255, g / 255, b / 255, a);
    CGContextSetRGBFillColor(c, r / 255, g / 255, b / 255, a);
}
static void line(CGContextRef c, double x0, double y0, double x1, double y1) {
    CGContextMoveToPoint(c, x0, y0); CGContextAddLineToPoint(c, x1, y1); CGContextStrokePath(c);
}
static void ellipse(CGContextRef c, double cx, double cy, double rx, double ry) {
    CGContextStrokeEllipseInRect(c, CGRectMake(cx - rx, cy - ry, 2 * rx, 2 * ry));
}

// All coordinates in icon pixels (0..48, y down); the context is pre-scaled.
static void draw(CGContextRef c) {
    // background: the app's dark panel with a soft vignette
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGFloat comps[] = { 30 / 255., 37 / 255., 46 / 255., 1,   12 / 255., 15 / 255., 19 / 255., 1 };
    CGGradientRef gr = CGGradientCreateWithColorComponents(cs, comps, NULL, 2);
    CGContextDrawRadialGradient(c, gr, CGPointMake(24, 22), 0, CGPointMake(24, 24), 34, kCGGradientDrawsAfterEndLocation);
    CGGradientRelease(gr); CGColorSpaceRelease(cs);

    CGContextSetLineCap(c, kCGLineCapRound);
    CGContextSetLineJoin(c, kCGLineJoinRound);

    // positions on the ring: top, lower left, lower right, and the star
    const double R = 15.5;
    double a[3] = { -90, 150, 30 }, sa = -30 * M_PI / 180;
    double px[3], py[3];
    for (int i = 0; i < 3; i++) { px[i] = 24 + R * cos(a[i] * M_PI / 180); py[i] = 24 + R * sin(a[i] * M_PI / 180); }

    // the ring, with gaps where the shapes and the star sit
    CGContextSaveGState(c);
    CGContextAddRect(c, CGRectMake(0, 0, 48, 48));
    for (int i = 0; i < 3; i++) CGContextAddEllipseInRect(c, CGRectMake(px[i] - 7.2, py[i] - 7.2, 14.4, 14.4));
    CGContextAddEllipseInRect(c, CGRectMake(24 + R * cos(sa) - 4, 24 + R * sin(sa) - 4, 8, 8));
    CGContextEOClip(c);
    rgb(c, 245, 245, 245, 1);
    CGContextSetLineWidth(c, 1.4);
    ellipse(c, 24, 24, R, R);
    CGContextRestoreGState(c);
    CGContextSetLineWidth(c, 1.25);

    // red wireframe cube
    {
        double x = px[0], y = py[0], s = 8, d = 3;   // front square side, depth offset
        double fx = x - s / 2 - d / 2, fy = y - s / 2 + d / 2;
        rgb(c, 235, 60, 55, 0.25);
        CGContextFillRect(c, CGRectMake(fx, fy, s, s));
        rgb(c, 240, 70, 60, 1);
        CGContextStrokeRect(c, CGRectMake(fx + d, fy - d, s, s));         // back
        CGContextStrokeRect(c, CGRectMake(fx, fy, s, s));                 // front
        line(c, fx, fy, fx + d, fy - d);         line(c, fx + s, fy, fx + s + d, fy - d);
        line(c, fx, fy + s, fx + d, fy + s - d); line(c, fx + s, fy + s, fx + s + d, fy + s - d);
    }
    // yellow wireframe sphere
    {
        double x = px[1], y = py[1], r = 5.6;
        rgb(c, 250, 210, 40, 0.22);
        CGContextFillEllipseInRect(c, CGRectMake(x - r, y - r, 2 * r, 2 * r));
        rgb(c, 252, 215, 50, 1);
        ellipse(c, x, y, r, r);
        CGContextSetLineWidth(c, 0.9);
        ellipse(c, x, y, r, r * 0.36);           // equator
        ellipse(c, x, y, r * 0.42, r);           // meridian
        CGContextSetLineWidth(c, 1.25);
    }
    // blue wireframe cylinder
    {
        double x = px[2], y = py[2], rx = 4.6, ry = 1.7, h = 8.5;
        double top = y - h / 2, bot = y + h / 2;
        rgb(c, 60, 120, 255, 0.25);
        CGContextFillRect(c, CGRectMake(x - rx, top, 2 * rx, h));
        rgb(c, 70, 130, 255, 1);
        ellipse(c, x, top, rx, ry);
        CGContextBeginPath(c);
        // bottom: front half solid
        CGContextSaveGState(c);
        CGContextTranslateCTM(c, x, bot); CGContextScaleCTM(c, 1, ry / rx);
        CGContextAddArc(c, 0, 0, rx, 0, M_PI, 0);
        CGContextRestoreGState(c);
        CGContextStrokePath(c);
        line(c, x - rx, top, x - rx, bot);
        line(c, x + rx, top, x + rx, bot);
    }
    // small white star riding the ring between cube and cylinder (the playhead)
    {
        double sx = 24 + R * cos(sa), sy = 24 + R * sin(sa), ro = 3.1, ri = 1.3;
        CGContextBeginPath(c);
        for (int k = 0; k < 10; k++) {
            double ang = -M_PI / 2 + k * M_PI / 5, rr = k % 2 ? ri : ro;
            if (k == 0) CGContextMoveToPoint(c, sx + rr * cos(ang), sy + rr * sin(ang));
            else CGContextAddLineToPoint(c, sx + rr * cos(ang), sy + rr * sin(ang));
        }
        CGContextClosePath(c);
        CGContextSetRGBFillColor(c, 1, 1, 1, 1);
        CGContextFillPath(c);
    }
}

static void render(const char *path, int scale) {
    int n = 48 * scale;
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef c = CGBitmapContextCreate(NULL, n, n, 8, 0, cs, kCGImageAlphaPremultipliedLast);
    CGContextTranslateCTM(c, 0, n); CGContextScaleCTM(c, scale, -scale);   // y down, icon pixels
    CGContextSetShouldAntialias(c, true);
    draw(c);
    CGImageRef img = CGBitmapContextCreateImage(c);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, strlen(path), false);
    CGImageDestinationRef d = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
    CGImageDestinationAddImage(d, img, NULL);
    CGImageDestinationFinalize(d);
    CFRelease(d); CFRelease(url); CGImageRelease(img); CGContextRelease(c); CGColorSpaceRelease(cs);
    printf("wrote %s\n", path);
}

// Centered single line of text; y is the baseline in a y-down context.
static void label(CGContextRef c, const char *str, const char *font, double size, double cx, double y,
                  double r, double g, double b) {
    CFStringRef fs = CFStringCreateWithCString(NULL, font, kCFStringEncodingUTF8);
    CTFontRef f = CTFontCreateWithName(fs, size, NULL);
    CGColorRef col = CGColorCreateGenericRGB(r / 255, g / 255, b / 255, 1);
    const void *keys[] = { kCTFontAttributeName, kCTForegroundColorAttributeName };
    const void *vals[] = { f, col };
    CFDictionaryRef attr = CFDictionaryCreate(NULL, keys, vals, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFStringRef ss = CFStringCreateWithCString(NULL, str, kCFStringEncodingUTF8);
    CFAttributedStringRef as = CFAttributedStringCreate(NULL, ss, attr);
    CTLineRef line = CTLineCreateWithAttributedString(as);
    double w = CTLineGetTypographicBounds(line, NULL, NULL, NULL);
    CGContextSaveGState(c);
    CGContextSetTextMatrix(c, CGAffineTransformMakeScale(1, -1));
    CGContextSetTextPosition(c, cx - w / 2, y);
    CTLineDraw(line, c);
    CGContextRestoreGState(c);
    CFRelease(line); CFRelease(as); CFRelease(ss); CFRelease(attr); CGColorRelease(col); CFRelease(f); CFRelease(fs);
}

// itch.io cover image (630x500): the icon artwork large, the name and a tagline.
static void render_cover(const char *path) {
    int W = 630, H = 500;
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef c = CGBitmapContextCreate(NULL, W, H, 8, 0, cs, kCGImageAlphaPremultipliedLast);
    CGContextTranslateCTM(c, 0, H); CGContextScaleCTM(c, 1, -1);
    CGContextSetRGBFillColor(c, 12 / 255., 15 / 255., 19 / 255., 1);
    CGContextFillRect(c, CGRectMake(0, 0, W, H));
    CGContextSaveGState(c);
    double sc = 7.0;
    CGContextTranslateCTM(c, W / 2.0 - 24 * sc, 6);
    CGContextScaleCTM(c, sc, sc);
    draw(c);
    CGContextRestoreGState(c);
    label(c, "nodeTrix", "Helvetica-Bold", 60, W / 2.0, 412, 245, 245, 245);
    label(c, "3D Euclidean sequencer + modular synth for 3DS homebrew", "Helvetica", 19, W / 2.0, 450, 170, 180, 190);
    label(c, "OSC  \xc2\xb7  REVERB  \xc2\xb7  GRAINS  \xc2\xb7  8 STEP SEQUENCERS", "Helvetica-Bold", 14, W / 2.0, 478, 46, 196, 160);
    CGImageRef img = CGBitmapContextCreateImage(c);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, strlen(path), false);
    CGImageDestinationRef d = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
    CGImageDestinationAddImage(d, img, NULL);
    CGImageDestinationFinalize(d);
    CFRelease(d); CFRelease(url); CGImageRelease(img); CGContextRelease(c); CGColorSpaceRelease(cs);
    printf("wrote %s\n", path);
}

int main(int argc, char **argv) {
    render(argc > 1 ? argv[1] : "icon.png", 1);
    render(argc > 2 ? argv[2] : "icon-preview.png", 8);
    if (argc > 3) render_cover(argv[3]);
    return 0;
}
