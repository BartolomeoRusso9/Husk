package android.graphics;

/**
 * A stream of frames from a producer (a Surface made on it: MediaCodec's video, lockCanvas drawing, an EGL window surface) to a
 * consumer. Husk keeps the frames in software: a back buffer the producer draws, and the front one the consumer (a TextureView)
 * shows. GL consumers (updateTexImage into an external texture) get no pixels yet.
 */
public class SurfaceTexture {
    public interface OnFrameAvailableListener { void onFrameAvailable(SurfaceTexture t); }
    public interface OnSetFrameRateListener { void onSetFrameRate(SurfaceTexture t, float rate, int compat, int strategy); }
    public static class OutOfResourcesException extends Exception { public OutOfResourcesException() {} public OutOfResourcesException(String s) { super(s); } }
    private int mTex;
    private final Object mLock = new Object();
    private int mW, mH;
    private Bitmap mFront, mBack, mSpare, mHeld;     /* mHeld: the frame the consumer last took */
    private int mPosted; private long mPostedSince; private boolean mSaidFirst;
    private Canvas mBackCanvas;
    /* One canvas per buffer, for as long as the buffer lives: a canvas flips its bitmap's drawing context when made and flips it back
       when freed, so a new canvas each frame on a buffer whose last one was not freed yet drew the frame upside down -- and right
       side up again whenever the collector got round to the old ones (the video turned over and back as it played). */
    private final java.util.IdentityHashMap<Bitmap, Canvas> mCanvases = new java.util.IdentityHashMap<>();
    private Canvas canvasFor(Bitmap b) { Canvas c = mCanvases.get(b); if (c == null) { c = new Canvas(b); mCanvases.put(b, c); } return c; }
    private void drop(Bitmap b) { Canvas c = mCanvases.remove(b); if (c != null) c.huskRelease(); }
    private boolean mLocked, mReleased;
    private long mTimestamp;
    private OnFrameAvailableListener mListener;
    private android.os.Handler mHandler;
    /** The TextureView showing this texture, told when a frame arrives. */
    public android.view.View huskView;
    public SurfaceTexture(int texName) { this(texName, false); }
    public SurfaceTexture(int texName, boolean singleBufferMode) { mTex = texName; }
    public SurfaceTexture(boolean singleBufferMode) { mTex = 0; }
    public void setOnFrameAvailableListener(OnFrameAvailableListener l) { setOnFrameAvailableListener(l, null); }
    public void setOnFrameAvailableListener(OnFrameAvailableListener l, android.os.Handler h) {
        mListener = l;
        mHandler = l == null ? null : h != null ? h : new android.os.Handler(android.os.Looper.myLooper() != null ? android.os.Looper.myLooper() : android.os.Looper.getMainLooper());
    }
    public void setOnSetFrameRateListener(OnSetFrameRateListener l, android.os.Handler h) {}
    public void setDefaultBufferSize(int w, int h) { synchronized (mLock) { mW = w; mH = h; } }
    /** The buffer size producers draw at (the consumer's size when it never set one). */
    public int[] huskSize() { synchronized (mLock) { return new int[] { mW, mH }; } }
    public Canvas huskLock(Rect dirty) {
        synchronized (mLock) {
            if (mReleased) return null;
            while (mLocked) { try { mLock.wait(); } catch (InterruptedException e) { return null; } if (mReleased) return null; }
            int w = mW > 0 ? mW : 1, h = mH > 0 ? mH : 1;
            /* never the frame the consumer is showing (it draws it on the main thread while this one is being drawn): a third
               buffer then, kept for the next time the consumer is behind -- the video flickered with cleared, half-drawn frames */
            if (mBack != null && mBack == mHeld) {
                Bitmap s = mSpare;
                mSpare = mBack;
                boolean fits = s != null && s.getWidth() == w && s.getHeight() == h;
                if (s != null && !fits) drop(s);
                mBack = fits && s != mFront ? s : null;
            }
            if (mBack == null || mBack.getWidth() != w || mBack.getHeight() != h) {
                if (mBack != null) drop(mBack);
                mBack = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
            }
            mBackCanvas = canvasFor(mBack);
            mLocked = true;
            mBackCanvas.save();
            if (dirty != null) mBackCanvas.clipRect(dirty);
            return mBackCanvas;
        }
    }
    public void huskPost(Canvas c) {
        synchronized (mLock) {
            if (!mLocked) return;
            mBackCanvas.restore();
            Bitmap f = mFront; mFront = mBack; mBack = f;
            if (mBack != null && (mBack.getWidth() != mFront.getWidth() || mBack.getHeight() != mFront.getHeight())) { drop(mBack); mBack = null; }
            mBackCanvas = mBack != null ? canvasFor(mBack) : null;
            mLocked = false; mTimestamp = System.nanoTime();
            mLock.notifyAll();
            mPosted++;
            if (!mSaidFirst) { mSaidFirst = true; android.util.Log.i("HuskVideo", "first frame, " + mFront.getWidth() + "x" + mFront.getHeight() + (huskView == null ? ", no view showing it" : "")); }
            if (mTimestamp - mPostedSince > 5_000_000_000L) {
                if (mPostedSince != 0) android.util.Log.i("HuskVideo", mPosted + " frames in " + (mTimestamp - mPostedSince) / 1_000_000 + " ms, " + mFront.getWidth() + "x" + mFront.getHeight() + (huskView == null ? ", no view showing them" : "") + ", from " + mSources + " @" + Integer.toHexString(System.identityHashCode(this)));
                mSources.clear();
                mPostedSince = mTimestamp; mPosted = 0;
            }
        }
        android.view.View v = huskView;
        if (v != null) v.postInvalidate();
        OnFrameAvailableListener l = mListener; android.os.Handler h = mHandler;
        if (l != null && h != null) h.post(() -> l.onFrameAvailable(this));
    }
    public boolean huskValid() { return !mReleased; }
    /* One producer at a time, as a BufferQueue connects one: a native window draws once it is connected, and a second window on
       the same texture is refused until the first lets go (Instagram's feed has the next video's decoder write to the shown one's
       surface for a while: its frames and the shown video's alternated). */
    private long mProducer;
    private int mRefused;
    public boolean huskConnect(long producer) {
        synchronized (mLock) {
            if (mProducer == 0 || mProducer == producer) { mProducer = producer; return true; }
            if (mRefused++ < 3) android.util.Log.i("HuskVideo", "a second producer for a texture that has one: refused, as Android does");
            return false;
        }
    }
    public void huskDisconnect(long producer) { synchronized (mLock) { if (mProducer == producer) mProducer = 0; } }
    private Bitmap mStage;
    /** A native producer's frame (husk-tl-egl.c's TextureView windows, ANativeWindow_lock): w x h RGBA at addr, bottom row first when flip. */
    private final java.util.HashMap<String, Integer> mSources = new java.util.HashMap<>();
    public void huskPostRgba(long addr, int w, int h, boolean flip) {
        synchronized (mLock) { String k = w + "x" + h + (flip ? " flipped" : ""); Integer n = mSources.get(k); mSources.put(k, n == null ? 1 : n + 1); }
        if (mStage == null || mStage.getWidth() != w || mStage.getHeight() != h) mStage = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
        mStage.huskSetRgba(addr);
        Canvas c = huskLock(null);
        if (c == null) return;
        try {
            c.save();
            if (flip) c.scale(1, -1, c.getWidth() / 2f, c.getHeight() / 2f);
            c.drawColor(0, PorterDuff.Mode.CLEAR);
            c.drawBitmap(mStage, null, new Rect(0, 0, c.getWidth(), c.getHeight()), null);
            c.restore();
        } finally { huskPost(c); }
    }
    /** The latest frame (null before the first). */
    public Bitmap huskFront() { synchronized (mLock) { mHeld = mFront; return mFront; } }
    public void updateTexImage() { if (mReleased) throw new IllegalStateException("updateTexImage on a released SurfaceTexture"); }
    public void releaseTexImage() {}
    public void detachFromGLContext() { mTex = 0; }
    public void attachToGLContext(int tex) { mTex = tex; }
    public void getTransformMatrix(float[] m) { android.opengl.Matrix.setIdentityM(m, 0); }
    public long getTimestamp() { return mTimestamp; }
    public int getDataSpace() { return 0; }
    public boolean isSingleBuffered() { return false; }
    public void release() { synchronized (mLock) { mReleased = true; mLock.notifyAll(); } }
    public boolean isReleased() { return mReleased; }
}
