//
// System.Web.Security.MembershipCreateUserException
//
// Authors:
//	Ben Maurer (bmaurer@users.sourceforge.net)
//
// (C) 2003 Ben Maurer
//

//
// Permission is hereby granted, free of charge, to any person obtaining
// a copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to
// the following conditions:
// 
// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
// LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
// OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
// WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//

#if NET_2_0
using System;
using System.Runtime.Serialization;

namespace System.Web.Security
{
	[Serializable]
	public class MembershipCreateUserException : Exception
	{
		MembershipCreateStatus statusCode;
		
		public MembershipCreateUserException ()
		{
		}
		
		public MembershipCreateUserException (string message): base (message)
		{
		}
		
		public MembershipCreateUserException (string message, Exception innerException): base (message, innerException)
		{
		}
		
		protected MembershipCreateUserException (SerializationInfo info, StreamingContext context): base (info, context)
		{
			info.AddValue ("statusCode", statusCode);
		}
		
		public MembershipCreateUserException (MembershipCreateStatus statusCode) : base (statusCode.ToString ())
		{
			this.statusCode = statusCode;
		}
		
		public override void GetObjectData (SerializationInfo info, StreamingContext ctx)
		{
			base.GetObjectData (info, ctx);
			statusCode = (MembershipCreateStatus) info.GetValue ("statusCode", typeof(MembershipCreateStatus));
		}
		
		public MembershipCreateStatus StatusCode {
			get { return statusCode; }
		}
	}
}
#endif

