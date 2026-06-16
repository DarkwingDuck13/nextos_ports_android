// MonoGame - Copyright (C) MonoGame Foundation, Inc
// This file is subject to the terms and conditions defined in
// file 'LICENSE.txt', which is part of this source code package.

using System;
using Microsoft.Xna.Framework.Graphics;

namespace Microsoft.Xna.Framework.Content
{
    [System.Diagnostics.CodeAnalysis.DynamicallyAccessedMembers(System.Diagnostics.CodeAnalysis.DynamicallyAccessedMemberTypes.All)]
    internal class Texture2DReader : ContentTypeReader<Texture2D>
    {
        [System.Runtime.InteropServices.DllImport("sor4astc")]
        static extern int sor4_astc_decode(byte[] data, ulong len, int w, int h, int bx, int by, byte[] outRGBA);
        static readonly int[,] AstcBlk = {{4,4},{5,4},{5,5},{6,5},{6,6},{8,5},{8,6},{8,8},{10,5},{10,6},{10,8},{10,10},{12,10},{12,12}};
        static readonly int Sor4TexScale = SOR4_GetScale();
        static int SOR4_GetScale(){ var v=System.Environment.GetEnvironmentVariable("SOR4_TEXSCALE"); int s; if(!string.IsNullOrEmpty(v)&&int.TryParse(v,out s)&&s>=1&&s<=4) return s; return 2; }
        // decodifica ASTC (w x h) -> RGBA8 e reduz por 'scale' (box filter) -> retorna (w/scale)x(h/scale)
        static byte[] Sor4DecodeAstc(byte[] data, int len, int w, int h, int scale) {
            int nb = len / 16; int bx = 0, by = 0;
            for (int i=0;i<AstcBlk.GetLength(0);i++){ int cbx=AstcBlk[i,0],cby=AstcBlk[i,1];
                if (((w+cbx-1)/cbx)*((h+cby-1)/cby) == nb){ bx=cbx; by=cby; break; } }
            var full = new byte[w*h*4];
            if (bx==0) { for(int p=0;p<full.Length;p+=4){full[p]=128;full[p+1]=128;full[p+2]=128;full[p+3]=255;} System.Console.Error.WriteLine("[ASTC] bloco?? len="+len+" "+w+"x"+h); }
            else { try { int rc = sor4_astc_decode(data, (ulong)len, w, h, bx, by, full);
                if (rc!=0){ System.Console.Error.WriteLine("[ASTC] decode rc="+rc); for(int p=0;p<full.Length;p+=4){full[p]=128;full[p+1]=128;full[p+2]=128;full[p+3]=255;} } }
                catch (System.Exception e){ System.Console.Error.WriteLine("[ASTC] EXC "+e.Message); } }
            // SOR4: atlas de fonte/mascara (RGB~0, A com conteudo) -> RGB=A (branco premult) p/ texto visivel
            {
                long sr=0,sg=0,sb=0,sa=0,np=full.Length/4;
                for(int p=0;p<full.Length;p+=4){sr+=full[p];sg+=full[p+1];sb+=full[p+2];sa+=full[p+3];}
                if (np>0 && (sr+sg+sb)/(np*3) < 8 && sa/np > 12) {
                    for(int p=0;p<full.Length;p+=4){ byte a=full[p+3]; full[p]=a; full[p+1]=a; full[p+2]=a; }
                    if(System.Environment.GetEnvironmentVariable("SOR4_TEXLOG")=="1") System.Console.Error.WriteLine($"[MASKFIX] {w}x{h} RGB=A");
                }
            }
            if (System.Environment.GetEnvironmentVariable("SOR4_DUMPTEX")=="1" && w<=400 && h<=400) {
                try {
                    // stats
                    long sr=0,sg=0,sb=0,sa=0; for(int p=0;p<full.Length;p+=4){sr+=full[p];sg+=full[p+1];sb+=full[p+2];sa+=full[p+3];}
                    int np=full.Length/4;
                    System.Console.Error.WriteLine($"[TEXDUMP] {w}x{h} avgR={sr/np} avgG={sg/np} avgB={sb/np} avgA={sa/np}");
                    System.IO.File.WriteAllBytes($"/tmp/tex_{w}x{h}.raw", full);
                } catch {}
            }
            if (scale<=1) return full;
            int nw=System.Math.Max(w/scale,1), nh=System.Math.Max(h/scale,1);
            var small=new byte[nw*nh*4];
            for(int y=0;y<nh;y++) for(int x=0;x<nw;x++){
                int sx=x*scale, sy=y*scale, r=0,g=0,b=0,a=0,cnt=0;
                for(int dy=0;dy<scale;dy++) for(int dx=0;dx<scale;dx++){
                    int px=sx+dx, py=sy+dy; if(px>=w||py>=h) continue;
                    int o=(py*w+px)*4; r+=full[o]; g+=full[o+1]; b+=full[o+2]; a+=full[o+3]; cnt++; }
                int d=(y*nw+x)*4; if(cnt==0)cnt=1;
                small[d]=(byte)(r/cnt); small[d+1]=(byte)(g/cnt); small[d+2]=(byte)(b/cnt); small[d+3]=(byte)(a/cnt);
            }
            return small;
        }
		public Texture2DReader()
		{
			// Do nothing
		}

        protected internal override Texture2D Read(ContentReader reader, Texture2D existingInstance)
		{
			Texture2D texture = null;

            var surfaceFormat = (SurfaceFormat)reader.ReadInt32();
            int width = reader.ReadInt32();
            int height = reader.ReadInt32();
            int levelCount = reader.ReadInt32();
            int levelCountOutput = levelCount;

            // If the system does not fully support Power of Two textures,
            // skip any mip maps supplied with any non PoT textures.
            if (levelCount > 1 && !reader.GetGraphicsDevice().GraphicsCapabilities.SupportsNonPowerOfTwo &&
                (!MathHelper.IsPowerOfTwo(width) || !MathHelper.IsPowerOfTwo(height)))
            {
                levelCountOutput = 1;
                System.Diagnostics.Debug.WriteLine(
                    "Device does not support non Power of Two textures. Skipping mipmaps.");
            }

			SurfaceFormat convertedFormat = surfaceFormat;
			switch (surfaceFormat)
			{
				case SurfaceFormat.Dxt1:
				case SurfaceFormat.Dxt1a:
					if (!reader.GetGraphicsDevice().GraphicsCapabilities.SupportsDxt1)
						convertedFormat = SurfaceFormat.Color;
					break;
				case SurfaceFormat.Dxt1SRgb:
					if (!reader.GetGraphicsDevice().GraphicsCapabilities.SupportsDxt1)
						convertedFormat = SurfaceFormat.ColorSRgb;
					break;
				case SurfaceFormat.Dxt3:
				case SurfaceFormat.Dxt5:
					if (!reader.GetGraphicsDevice().GraphicsCapabilities.SupportsS3tc)
						convertedFormat = SurfaceFormat.Color;
					break;
				case SurfaceFormat.Dxt3SRgb:
				case SurfaceFormat.Dxt5SRgb:
					if (!reader.GetGraphicsDevice().GraphicsCapabilities.SupportsS3tc)
						convertedFormat = SurfaceFormat.ColorSRgb;
					break;
				case SurfaceFormat.NormalizedByte4:
					convertedFormat = SurfaceFormat.Color;
					break;
			}
			
            int sor4Scale = 1;
            if ((int)surfaceFormat >= 96) { convertedFormat = SurfaceFormat.Color; sor4Scale = Sor4TexScale; levelCountOutput = 1; } // SOR4: ASTC -> RGBA reduzido (memoria)
            if (surfaceFormat == SurfaceFormat.Bgra4444) { convertedFormat = SurfaceFormat.Color; levelCountOutput = 1; } // SOR4: 4444 -> RGBA8 (fonte/Mali)
            int sor4TexW = System.Math.Max(width/sor4Scale,1), sor4TexH = System.Math.Max(height/sor4Scale,1);
            texture = existingInstance ?? new Texture2D(reader.GetGraphicsDevice(), sor4TexW, sor4TexH, levelCountOutput > 1, convertedFormat);
#if OPENGL
            Threading.BlockOnUIThread(() =>
            {
#endif
                for (int level = 0; level < levelCount; level++)
			    {
				    var levelDataSizeInBytes = reader.ReadInt32();
                    var levelData = ContentManager.ScratchBufferPool.Get(levelDataSizeInBytes);
                    reader.Read(levelData, 0, levelDataSizeInBytes);
                    int levelWidth = Math.Max(width >> level, 1);
                    int levelHeight = Math.Max(height >> level, 1);

                    if (level >= levelCountOutput)
                        continue;

                    if ((int)surfaceFormat >= 96) {
                        // SOR4: ASTC -> decode p/ RGBA8 via astcenc (Mali-450 nao tem ASTC nativo)
                        levelData = Sor4DecodeAstc(levelData, levelDataSizeInBytes, levelWidth, levelHeight, sor4Scale);
                        levelDataSizeInBytes = levelData.Length;
                    }
				    //Convert the image data if required
				    switch (surfaceFormat)
				    {
					    case SurfaceFormat.Dxt1:
                        case SurfaceFormat.Dxt1SRgb:
                        case SurfaceFormat.Dxt1a:
				            if (!reader.GetGraphicsDevice().GraphicsCapabilities.SupportsDxt1 && convertedFormat == SurfaceFormat.Color)
				            {
				                levelData = DxtUtil.DecompressDxt1(levelData, levelWidth, levelHeight);
				                levelDataSizeInBytes = levelData.Length;
				            }
				            break;
					    case SurfaceFormat.Dxt3:
					    case SurfaceFormat.Dxt3SRgb:
                            if (!reader.GetGraphicsDevice().GraphicsCapabilities.SupportsS3tc)
				                if (!reader.GetGraphicsDevice().GraphicsCapabilities.SupportsS3tc &&
				                    convertedFormat == SurfaceFormat.Color)
				                {
				                    levelData = DxtUtil.DecompressDxt3(levelData, levelWidth, levelHeight);
                                    levelDataSizeInBytes = levelData.Length;
                                }
				            break;
					    case SurfaceFormat.Dxt5:
					    case SurfaceFormat.Dxt5SRgb:
                            if (!reader.GetGraphicsDevice().GraphicsCapabilities.SupportsS3tc)
				                if (!reader.GetGraphicsDevice().GraphicsCapabilities.SupportsS3tc &&
				                    convertedFormat == SurfaceFormat.Color)
				                {
				                    levelData = DxtUtil.DecompressDxt5(levelData, levelWidth, levelHeight);
                                    levelDataSizeInBytes = levelData.Length;
                                }
				            break;
                        case SurfaceFormat.Bgra5551:
                            {
#if OPENGL
                                // Shift the channels to suit OpenGL
                                int offset = 0;
                                for (int y = 0; y < levelHeight; y++)
                                {
                                    for (int x = 0; x < levelWidth; x++)
                                    {
                                        ushort pixel = BitConverter.ToUInt16(levelData, offset);
                                        pixel = (ushort)(((pixel & 0x7FFF) << 1) | ((pixel & 0x8000) >> 15));
                                        levelData[offset] = (byte)(pixel);
                                        levelData[offset + 1] = (byte)(pixel >> 8);
                                        offset += 2;
                                    }
                                }
#endif
                            }
                            break;
					    case SurfaceFormat.Bgra4444:
						    {
                                // SOR4: decodifica BGRA4444 -> RGBA8 (Color) p/ Mali-450
                                var rgba = new byte[levelWidth*levelHeight*4];
                                for (int i=0;i<levelWidth*levelHeight;i++){
                                    ushort p = BitConverter.ToUInt16(levelData, i*2);
                                    int b=(p)&0xF, g=(p>>4)&0xF, r=(p>>8)&0xF, a=(p>>12)&0xF;
                                    rgba[i*4]=(byte)(r*17); rgba[i*4+1]=(byte)(g*17); rgba[i*4+2]=(byte)(b*17); rgba[i*4+3]=(byte)(a*17);
                                }
                                if(System.Environment.GetEnvironmentVariable("SOR4_DUMPTEX")=="1" && levelWidth<=600){ try{ System.IO.File.WriteAllBytes($"/tmp/font_{levelWidth}x{levelHeight}.raw", rgba);}catch{} }
                                levelData = rgba; levelDataSizeInBytes = rgba.Length;
						    }
						    break;
					    case SurfaceFormat.NormalizedByte4:
						    {
							    int bytesPerPixel = surfaceFormat.GetSize();
							    int pitch = levelWidth * bytesPerPixel;
							    for (int y = 0; y < levelHeight; y++)
							    {
								    for (int x = 0; x < levelWidth; x++)
								    {
									    int color = BitConverter.ToInt32(levelData, y * pitch + x * bytesPerPixel);
									    levelData[y * pitch + x * 4] = (byte)(((color >> 16) & 0xff)); //R:=W
									    levelData[y * pitch + x * 4 + 1] = (byte)(((color >> 8) & 0xff)); //G:=V
									    levelData[y * pitch + x * 4 + 2] = (byte)(((color) & 0xff)); //B:=U
									    levelData[y * pitch + x * 4 + 3] = (byte)(((color >> 24) & 0xff)); //A:=Q
								    }
							    }
						    }
						    break;
				    }
				
                    texture.SetData(level, null, levelData, 0, levelDataSizeInBytes);
                    ContentManager.ScratchBufferPool.Return(levelData);
			    }
#if OPENGL
            });
#endif
        			
			return texture;
		}
    }
}
